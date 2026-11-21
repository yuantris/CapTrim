//! The v1 effect vocabulary: one list of names shared by preview and export.
//!
//! Preview renders these in the GL compositor's fragment shader; export
//! translates them into an FFmpeg filter chain with [`effects_to_filter_desc`].
//! Both sides apply the names in [`CANONICAL_ORDER`], so a clip's effect list
//! means the same thing everywhere no matter what order the platform inserted
//! them in. The list behaves as a **set**: duplicates collapse to one pass,
//! because the GL side carries them as flags and cannot run a second pass.

use crate::error::{Error, Result};

/// v1 effect names, in the order both backends must apply them.
///
/// Geometric flips first (they move pixels), colour operations in the middle,
/// blur last (the colour maths must not re-sample an already-blurred image).
///
/// Every name here must have BOTH an FFmpeg filter part in
/// [`effects_to_filter_desc`] and a GLSL branch in the compositor; the exact
/// constants are a cross-backend contract documented in the README「滤镜」
/// section. Adding a name without both implementations is a bug, not a
/// half-feature.
pub const CANONICAL_ORDER: [&str; 8] = [
    "hflip",
    "vflip",
    "brightness",
    "contrast",
    "saturation",
    "grayscale",
    "invert",
    "blur",
];

/// Bit for each effect in the binary frame plan. The bit index is the effect's
/// position in [`CANONICAL_ORDER`]; the C header (`CAPTRIM_EFFECT_*`) and the
/// GLSL compositor must agree with these exactly. A `u32` is enough for the
/// eight v1 names and keeps `captrim_layer_desc_t` a scalar-only POD.
pub const EFFECT_HFLIP: u32 = 1 << 0;
pub const EFFECT_VFLIP: u32 = 1 << 1;
pub const EFFECT_BRIGHTNESS: u32 = 1 << 2;
pub const EFFECT_CONTRAST: u32 = 1 << 3;
pub const EFFECT_SATURATION: u32 = 1 << 4;
pub const EFFECT_GRAYSCALE: u32 = 1 << 5;
pub const EFFECT_INVERT: u32 = 1 << 6;
pub const EFFECT_BLUR: u32 = 1 << 7;

const EFFECT_BITS: [u32; 8] = [
    EFFECT_HFLIP,
    EFFECT_VFLIP,
    EFFECT_BRIGHTNESS,
    EFFECT_CONTRAST,
    EFFECT_SATURATION,
    EFFECT_GRAYSCALE,
    EFFECT_INVERT,
    EFFECT_BLUR,
];

/// Pack a clip's effect list into the bit mask carried by
/// `captrim_layer_desc_t.effects`. Duplicates naturally collapse, matching the
/// set semantics of the vocabulary. Unknown names are ignored here (the model
/// rejects them at edit time); the mask is a rendering hint, not a validator.
pub fn effects_mask(effects: &[String]) -> u32 {
    let mut mask = 0u32;
    for (i, name) in CANONICAL_ORDER.iter().enumerate() {
        if effects.iter().any(|effect| effect == name) {
            mask |= EFFECT_BITS[i];
        }
    }
    mask
}

/// Is `name` part of the v1 vocabulary? [`crate::model::Clip::validate`]
/// rejects anything else, so a draft can never carry an effect that no
/// backend renders.
pub fn is_known_effect(name: &str) -> bool {
    CANONICAL_ORDER.contains(&name)
}

/// The FFmpeg side of one effect, in the exact form `ct_ff_transcode`'s
/// `filter_desc` expects. `None` means the name is not in the vocabulary.
///
/// The numeric constants are the shared contract with the GLSL side:
///
/// | effect     | FFmpeg            | GLSL (compositor)                     |
/// |------------|-------------------|---------------------------------------|
/// | `hflip`    | `hflip`           | mirror local x                        |
/// | `vflip`    | `vflip`           | mirror local y                        |
/// | `brightness` | `eq=brightness=0.15` | `c + 0.15`                         |
/// | `contrast` | `eq=contrast=1.30`   | `(c - 0.5) * 1.30 + 0.5`           |
/// | `saturation` | `eq=saturation=1.30` | `mix(luma, c, 1.30)`              |
/// | `grayscale`  | `hue=s=0`          | `vec3(luma)`                        |
/// | `invert`     | `negate`           | `1.0 - c`                            |
/// | `blur`       | `boxblur=2:1`      | 5×5 box (radius 2, one pass per axis) |
///
/// FFmpeg works in YUV and the shader in RGB, so the two are deliberately
/// *approximate* to each other, not bit-identical — the same trade-off the
/// `scale`/`fit` path already makes. What must match is which effects run and
/// in which order.
fn filter_part(name: &str) -> Option<&'static str> {
    Some(match name {
        "hflip" => "hflip",
        "vflip" => "vflip",
        "brightness" => "eq=brightness=0.15",
        "contrast" => "eq=contrast=1.30",
        "saturation" => "eq=saturation=1.30",
        "grayscale" => "hue=s=0",
        "invert" => "negate",
        "blur" => "boxblur=2:1",
        _ => return None,
    })
}

/// The FFmpeg filter that implements one effect.
///
/// This is what makes the licence profile visible to the caller: `eq` and
/// `boxblur` are **GPL filters inside FFmpeg itself** (`eq_filter_deps="gpl"`,
/// `boxblur_filter_deps="gpl"` in FFmpeg's configure), so an LGPL build has no
/// brightness / contrast / saturation / blur at all. `hflip`, `vflip`,
/// `hue` and `negate` are LGPL and survive. Without this look-up the four
/// effects would fail deep inside `avfilter_graph_parse_ptr` with a bare
/// "No such filter" long after the export started.
pub fn ffmpeg_filter(name: &str) -> Option<&'static str> {
    Some(match name {
        "hflip" => "hflip",
        "vflip" => "vflip",
        "brightness" | "contrast" | "saturation" => "eq",
        "grayscale" => "hue",
        "invert" => "negate",
        "blur" => "boxblur",
        _ => return None,
    })
}

/// Effects this build can actually render, in canonical order.
pub fn available_effects(is_filter_available: &dyn Fn(&str) -> bool) -> Vec<&'static str> {
    CANONICAL_ORDER
        .iter()
        .copied()
        .filter(|name| ffmpeg_filter(name).map(is_filter_available) == Some(true))
        .collect()
}

/// Translate a clip's effect list into an FFmpeg filter chain for
/// `ct_ff_transcode`'s `filter_desc`, emitted in [`CANONICAL_ORDER`] with
/// duplicates collapsed. Returns an empty string when there is nothing to
/// apply, so callers can concatenate unconditionally.
///
/// Unknown names are an error rather than a silent skip: `Clip::validate`
/// already rejects them at edit time, so reaching this function with one
/// means a caller bypassed the model.
pub fn effects_to_filter_desc(effects: &[String]) -> Result<String> {
    effects_to_filter_desc_with(effects, &|_| true)
}

/// [`effects_to_filter_desc`] against a build whose filter set is known.
///
/// An effect whose FFmpeg filter is absent returns [`Error::Unsupported`]
/// naming both the effect and the filter, plus the effects that do work, so an
/// app can offer only what this build can render instead of failing mid-export.
/// A build missing `eq` (LGPL profile) therefore loses brightness, contrast and
/// saturation — a real loss, reported as one.
pub fn effects_to_filter_desc_with(
    effects: &[String],
    is_filter_available: &dyn Fn(&str) -> bool,
) -> Result<String> {
    for name in effects {
        if !is_known_effect(name) {
            return Err(Error::invalid(format!("unknown effect: {name}")));
        }
        let filter = ffmpeg_filter(name).expect("known effects all have a filter");
        if !is_filter_available(filter) {
            let usable = available_effects(is_filter_available);
            return Err(Error::Unsupported(format!(
                "effect '{name}' needs the FFmpeg '{filter}' filter, which is GPL-only \
                 and absent from this build; available effects: {}",
                if usable.is_empty() {
                    "none".to_string()
                } else {
                    usable.join(",")
                }
            )));
        }
    }
    let mut parts: Vec<&'static str> = Vec::new();
    for name in CANONICAL_ORDER {
        if effects.iter().any(|effect| effect == name) {
            // Known names all have a part; `filter_part` mirrors the table.
            if let Some(part) = filter_part(name) {
                parts.push(part);
            }
        }
    }
    Ok(parts.join(","))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn names(input: &[&str]) -> Vec<String> {
        input.iter().map(|name| name.to_string()).collect()
    }

    #[test]
    fn empty_effects_produce_an_empty_chain() {
        assert_eq!(effects_to_filter_desc(&[]).unwrap(), "");
    }

    #[test]
    fn chain_follows_canonical_order_not_insertion_order() {
        // Scrambled in, canonical out: geometry first, blur last.
        let desc = effects_to_filter_desc(&names(&["blur", "grayscale", "hflip"])).unwrap();
        assert_eq!(desc, "hflip,hue=s=0,boxblur=2:1");
    }

    #[test]
    fn duplicates_collapse_because_the_shader_side_is_flag_based() {
        let desc = effects_to_filter_desc(&names(&["invert", "invert", "brightness"])).unwrap();
        assert_eq!(desc, "eq=brightness=0.15,negate");
    }

    #[test]
    fn every_canonical_name_translates() {
        for name in CANONICAL_ORDER {
            let desc = effects_to_filter_desc(&names(&[name])).unwrap();
            assert!(!desc.is_empty(), "{name} must have an FFmpeg filter part");
            assert!(!desc.contains(' '), "{name} must not need quoting: {desc}");
        }
    }

    #[test]
    fn unknown_names_are_an_error_not_a_silent_skip() {
        let err = effects_to_filter_desc(&names(&["sepia"])).unwrap_err();
        assert!(err.to_string().contains("sepia"), "{err}");
        assert_eq!(err.code(), -1);
    }

    #[test]
    fn mask_bit_matches_canonical_position() {
        // The C header and GLSL use these exact bit values; pin them here so a
        // reorder of CANONICAL_ORDER cannot silently desync the backends.
        assert_eq!(effects_mask(&names(&["hflip"])), 1 << 0);
        assert_eq!(effects_mask(&names(&["vflip"])), 1 << 1);
        assert_eq!(effects_mask(&names(&["brightness"])), 1 << 2);
        assert_eq!(effects_mask(&names(&["contrast"])), 1 << 3);
        assert_eq!(effects_mask(&names(&["saturation"])), 1 << 4);
        assert_eq!(effects_mask(&names(&["grayscale"])), 1 << 5);
        assert_eq!(effects_mask(&names(&["invert"])), 1 << 6);
        assert_eq!(effects_mask(&names(&["blur"])), 1 << 7);
    }

    #[test]
    fn mask_collapses_duplicates_and_ignores_order() {
        let mask = effects_mask(&names(&["blur", "hflip", "blur", "invert"]));
        assert_eq!(mask, EFFECT_HFLIP | EFFECT_INVERT | EFFECT_BLUR);
        assert_eq!(effects_mask(&[]), 0);
    }

    #[test]
    fn every_canonical_name_names_the_filter_it_needs() {
        for name in CANONICAL_ORDER {
            assert!(ffmpeg_filter(name).is_some(), "{name} must name a filter");
        }
        assert_eq!(ffmpeg_filter("blur"), Some("boxblur"));
        assert_eq!(ffmpeg_filter("brightness"), Some("eq"));
        assert_eq!(ffmpeg_filter("sepia"), None);
    }

    /// The LGPL profile is built without FFmpeg's GPL filters (`eq`, `boxblur`).
    /// Those four effects must say so by name instead of producing a graph that
    /// libavfilter rejects later with "No such filter".
    #[test]
    fn gpl_only_filters_make_the_effect_unsupported() {
        // Filters an LGPL build still has; `eq` and `boxblur` are missing.
        let lgpl = |filter: &str| !matches!(filter, "eq" | "boxblur");
        assert_eq!(
            available_effects(&lgpl),
            vec!["hflip", "vflip", "grayscale", "invert"]
        );

        let err = effects_to_filter_desc_with(&names(&["brightness"]), &lgpl).unwrap_err();
        assert!(err.to_string().contains("brightness"), "{err}");
        assert!(err.to_string().contains("eq"), "{err}");
        assert_eq!(err.code(), -11);

        let err = effects_to_filter_desc_with(&names(&["blur"]), &lgpl).unwrap_err();
        assert!(err.to_string().contains("boxblur"), "{err}");

        // Everything LGPL keeps still translates; the same chain the GPL build
        // emits, minus the two GPL parts.
        assert_eq!(
            effects_to_filter_desc_with(&names(&["grayscale", "hflip"]), &lgpl).unwrap(),
            "hflip,hue=s=0"
        );
    }

    /// A full GPL build must produce exactly the historical chain: the
    /// availability check may not change a single byte of what export emits.
    #[test]
    fn gpl_build_chain_is_unchanged() {
        let gpl = |_: &str| true;
        assert_eq!(
            effects_to_filter_desc_with(
                &names(&["blur", "grayscale", "hflip", "brightness"]),
                &gpl
            )
            .unwrap(),
            "hflip,eq=brightness=0.15,hue=s=0,boxblur=2:1"
        );
    }
}
