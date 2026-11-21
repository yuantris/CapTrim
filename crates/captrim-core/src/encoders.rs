//! Which software encoders this FFmpeg build actually ships.
//!
//! The SDK is built in two licence profiles (see scripts/ffmpeg-config-common.sh):
//! a GPL profile with libx264 / libx265, and an LGPL profile without them, which a
//! closed-source app can distribute. Both profiles share every other component —
//! decoders, filters, muxers, AAC — so the only thing an app can notice is that
//! `libx264` is gone.
//!
//! Failing at encode time with "Encoder not found" is the wrong answer: the
//! timeline was fine, the build simply cannot encode that codec. So the SDK asks
//! the backend what it has, resolves `""` (auto) to a real encoder, and refuses
//! an unavailable request up front, listing what *is* available.

use crate::{Error, Result};

/// Software encoders the SDK knows how to ask FFmpeg for, in preference order.
///
/// `libx264` / `libx264rgb` / `libx265` are GPL-only; `libopenh264` is BSD
/// (patent terms come from its own distribution, not from this list); `mpeg4`
/// is FFmpeg's native encoder and exists in every profile.
pub const KNOWN_ENCODERS: [&str; 5] = ["libx264", "libx264rgb", "libx265", "libopenh264", "mpeg4"];

/// Codec an encoder name produces, as `probe` reports it.
pub fn codec_of(encoder: &str) -> &'static str {
    match encoder {
        "libx264" | "libx264rgb" | "libopenh264" => "h264",
        "libx265" => "hevc",
        "mpeg4" => "mpeg4",
        _ => "unknown",
    }
}

/// Whether this encoder can only exist in a GPL-configured FFmpeg.
pub fn needs_gpl(encoder: &str) -> bool {
    matches!(encoder, "libx264" | "libx264rgb" | "libx265")
}

/// Software encoders available in this build, filtered from [`KNOWN_ENCODERS`].
pub fn available(has: &dyn Fn(&str) -> bool) -> Vec<&'static str> {
    KNOWN_ENCODERS
        .iter()
        .copied()
        .filter(|name| has(name))
        .collect()
}

/// Turn a caller's `codec` argument into the encoder name to hand FFmpeg.
///
/// * `""` (or unknown empty-ish input) means "H.264, whatever this build has":
///   `libx264` first, then `libopenh264`. It never silently downgrades to a
///   different *codec* — only to a different encoder for the same codec.
/// * A concrete name is accepted only if this build has it. Otherwise the error
///   names the request and lists the encoders that do exist, which is what an
///   app should show instead of a failed export.
pub fn resolve(codec: &str, has: &dyn Fn(&str) -> bool) -> Result<String> {
    let request = codec.trim();
    if request.is_empty() {
        for candidate in ["libx264", "libopenh264"] {
            if has(candidate) {
                return Ok(candidate.to_string());
            }
        }
        return Err(Error::Unsupported(format!(
            "this FFmpeg build has no H.264 software encoder (LGPL profile without \
             libx264/libopenh264); available: {}",
            list(has)
        )));
    }
    if has(request) {
        return Ok(request.to_string());
    }
    Err(Error::Unsupported(format!(
        "encoder '{request}' is not in this FFmpeg build ({} profile); available: {}",
        if needs_gpl(request) {
            "GPL-only"
        } else {
            "unknown"
        },
        list(has)
    )))
}

fn list(has: &dyn Fn(&str) -> bool) -> String {
    let names = available(has);
    if names.is_empty() {
        "none".to_string()
    } else {
        names.join(", ")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn gpl_build(name: &str) -> bool {
        matches!(name, "libx264" | "libx264rgb" | "libx265" | "mpeg4" | "aac")
    }

    fn lgpl_build(name: &str) -> bool {
        matches!(name, "mpeg4" | "aac")
    }

    #[test]
    fn auto_prefers_x264_then_openh264() {
        let both = |name: &str| gpl_build(name) || name == "libopenh264";
        assert_eq!(resolve("", &both).unwrap(), "libx264");
        let only_open = |name: &str| name == "libopenh264";
        assert_eq!(resolve("", &only_open).unwrap(), "libopenh264");
    }

    #[test]
    fn auto_never_switches_codec() {
        // No H.264 encoder at all: refuse rather than quietly emit mpeg4,
        // because the caller asked for a timeline export, not for mpeg4.
        let err = resolve("", &lgpl_build).unwrap_err();
        assert!(
            err.to_string().contains("no H.264 software encoder"),
            "{err}"
        );
        assert!(err.to_string().contains("mpeg4"), "{err}");
    }

    #[test]
    fn explicit_gpl_encoder_is_refused_in_an_lgpl_build() {
        let err = resolve("libx264", &lgpl_build).unwrap_err();
        assert!(err.to_string().contains("GPL-only"), "{err}");
        assert!(err.to_string().contains("available: mpeg4"), "{err}");
    }

    #[test]
    fn explicit_encoder_that_exists_is_returned_as_given() {
        assert_eq!(resolve("libx265", &gpl_build).unwrap(), "libx265");
        assert_eq!(resolve("mpeg4", &lgpl_build).unwrap(), "mpeg4");
        assert_eq!(codec_of("libopenh264"), "h264");
        assert!(needs_gpl("libx265") && !needs_gpl("libopenh264"));
    }
}
