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

/// Translate a clip's effect list into an FFmpeg filter chain for
/// `ct_ff_transcode`'s `filter_desc`, emitted in [`CANONICAL_ORDER`] with
/// duplicates collapsed. Returns an empty string when there is nothing to
/// apply, so callers can concatenate unconditionally.
///
/// Unknown names are an error rather than a silent skip: `Clip::validate`
/// already rejects them at edit time, so reaching this function with one
/// means a caller bypassed the model.
pub fn effects_to_filter_desc(effects: &[String]) -> Result<String> {
    for name in effects {
        if !is_known_effect(name) {
            return Err(Error::invalid(format!("unknown effect: {name}")));
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
}
