//! Export-plan ABI.
//!
//! The platform does not need to know how a timeline becomes segments; it only
//! needs a stable JSON snapshot built from the same Rust core that drives the
//! preview. Returning JSON keeps the C ABI small while still letting a host
//! app cache the plan and walk frames without a cross-boundary call each time.

use crate::{
    captrim_engine_t, cstr, engine_from, ffi_guard, lock, owned_cstring, CaptrimProgressCb,
};
use captrim_core::{
    compile_export_graph, effects_to_filter_desc_with, plan_export, Error, GraphSettings,
};
use std::os::raw::{c_char, c_void};
use std::ptr;

/// Export the whole timeline into one file (M1 "can deliver a file").
///
/// The core compiles the timeline's `ExportPlan` into a single FFmpeg
/// `filter_complex` over every distinct source (segments, layers, transitions,
/// audio tracks) and the backend runs it: N demuxers, one graph, `vout` encoded
/// with `video_codec`, `aout` (when the project has audio) encoded as AAC.
/// Preview and export read the same plan, so what was cut is what gets written.
///
/// `video_codec` may be NULL or "" for libx264; `crf` <= 0 uses 20; `preset`
/// NULL/"" uses the backend default; `audio_bitrate_kbps` <= 0 uses 192.
/// `progress` returns non-zero to cancel, which deletes the partial file.
/// Returns CAPTRIM_ERR_NO_BACKEND when built without the `ffmpeg` feature.
#[no_mangle]
pub extern "C" fn captrim_engine_export_timeline(
    engine: *mut captrim_engine_t,
    dst_uri: *const c_char,
    work_dir: *const c_char,
    video_codec: *const c_char,
    crf: i32,
    preset: *const c_char,
    audio_bitrate_kbps: i32,
    progress: CaptrimProgressCb,
    user_data: *mut c_void,
) -> i32 {
    ffi_guard(|| {
        let dst = unsafe { cstr(dst_uri) }?.to_string();
        let work_dir = if work_dir.is_null() {
            None
        } else {
            let text = unsafe { cstr(work_dir) }?.to_string();
            if text.trim().is_empty() {
                None
            } else {
                Some(std::path::PathBuf::from(text))
            }
        };
        let requested_codec = if video_codec.is_null() {
            String::new()
        } else {
            unsafe { cstr(video_codec) }?.to_string()
        };
        // "" means H.264 with whatever this build has; a named encoder is
        // checked against the shipped FFmpeg. Either way the answer is decided
        // before a frame is decoded, so an LGPL build reports what it lacks
        // instead of dying inside the encoder open.
        let backend = crate::default_backend();
        let codec = captrim_core::encoders::resolve(&requested_codec, &|name| {
            backend.encoder_available(name)
        })?;
        let preset = if preset.is_null() {
            String::new()
        } else {
            unsafe { cstr(preset) }?.to_string()
        };
        let e = engine_from(engine)?;
        // A host that called `captrim_engine_set_text_font` gets exactly that
        // font; otherwise the core resolves the platform default lazily, only
        // when the plan actually carries a caption.
        let still_font = e
            .text_font
            .lock()
            .map_err(|_| Error::Internal("font mutex poisoned".into()))?
            .clone();
        let graph = {
            let settings = GraphSettings {
                codec,
                crf: if crf > 0 { crf.min(51) as u8 } else { 20 },
                preset,
                audio_bitrate_kbps: if audio_bitrate_kbps > 0 {
                    audio_bitrate_kbps as u32
                } else {
                    192
                },
                // The graph already runs at the timeline's exact frame rate; no
                // extra re-timing is added here.
                filter_suffix: String::new(),
                still_dir: work_dir,
                still_font,
            };
            let editor = lock(&e.editor)?;
            let timeline = editor.timeline();
            let plan = plan_export(timeline);
            // The graph compiler translates effects itself, but a filter this
            // build lacks would then surface as a libavfilter parse error after
            // the demuxers already opened. Decide up front, per effect.
            for segment in &plan.segments {
                for item in &segment.video {
                    effects_to_filter_desc_with(&item.effects, &|filter| {
                        backend.filter_available(filter)
                    })?;
                }
            }
            compile_export_graph(timeline, &plan, &settings)?
        };
        let mut report = |value: f32| match progress {
            Some(cb) => unsafe { cb(value, user_data) == 0 },
            None => true,
        };
        e.backend.export_graph(&graph, &dst, &mut report)?;
        Ok(0)
    })
}

/// Full export breakdown as an `ExportPlan` JSON object.
#[no_mangle]
pub extern "C" fn captrim_engine_export_plan(
    engine: *mut captrim_engine_t,
    out_json: *mut *mut c_char,
) -> i32 {
    ffi_guard(|| {
        if out_json.is_null() {
            return Err(Error::invalid("out_json is null"));
        }
        unsafe { *out_json = ptr::null_mut() };
        let e = engine_from(engine)?;
        let json = {
            let editor = lock(&e.editor)?;
            plan_export(editor.timeline()).to_json()?
        };
        unsafe { *out_json = owned_cstring(json)? };
        Ok(0)
    })
}

/// Translate a clip's `effects` list (a JSON array of names, exactly as it
/// appears in `ExportVideoItem.effects`) into the FFmpeg `filter_desc` chain
/// for `captrim_engine_export_transcode`.
///
/// Keeping the translation in the core means the platform never duplicates the
/// effect vocabulary or its constants: it reads `effects` from its cached
/// export plan, hands the array here, and passes the returned string to the
/// transcode call. An empty array yields an empty string, which transcode
/// treats as a passthrough. Release `out_desc` with `captrim_string_free()`.
#[no_mangle]
pub extern "C" fn captrim_effects_to_filter_desc(
    effects_json: *const c_char,
    out_desc: *mut *mut c_char,
) -> i32 {
    ffi_guard(|| {
        if out_desc.is_null() {
            return Err(Error::invalid("out_desc is null"));
        }
        unsafe { *out_desc = ptr::null_mut() };
        let json = unsafe { cstr(effects_json)? };
        let effects: Vec<String> = serde_json::from_str(json)
            .map_err(|e| Error::invalid(format!("effects must be a JSON string array: {e}")))?;
        // Doubles as the capability probe for the effect vocabulary: an LGPL
        // build has no `eq` / `boxblur` (GPL filters inside FFmpeg), so
        // brightness / contrast / saturation / blur come back as -11 with the
        // effects that do work, instead of a graph that fails at parse time.
        let backend = crate::default_backend();
        let desc =
            effects_to_filter_desc_with(&effects, &|filter| backend.filter_available(filter))?;
        unsafe { *out_desc = owned_cstring(desc)? };
        Ok(0)
    })
}

/// Whether this build can encode with `name` (`libx264`, `libx265`, `mpeg4`,
/// ...). Lets an app offer only encoders that exist instead of discovering it
/// when an export fails. Returns 1 available, 0 not.
#[no_mangle]
pub extern "C" fn captrim_encoder_available(name: *const c_char) -> i32 {
    ffi_guard(|| {
        if name.is_null() {
            return Ok(0);
        }
        let name = unsafe { cstr(name) }?;
        let backend = crate::default_backend();
        Ok(if backend.encoder_available(name) {
            1
        } else {
            0
        })
    })
}

/// Software encoders this build ships, comma separated (may be empty).
/// Caller frees the string with `captrim_string_free`.
#[no_mangle]
pub extern "C" fn captrim_encoders(out: *mut *mut c_char) -> i32 {
    ffi_guard(|| {
        if out.is_null() {
            return Err(Error::invalid("out is null"));
        }
        unsafe { *out = ptr::null_mut() };
        let backend = crate::default_backend();
        let names = captrim_core::encoders::available(&|name| backend.encoder_available(name));
        let text = names.join(",");
        unsafe { *out = owned_cstring(text)? };
        Ok(0)
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::captrim_string_free;
    use std::ffi::CString;

    fn translate(json: &str) -> Result<String, i32> {
        let input = CString::new(json).unwrap();
        let mut out: *mut c_char = ptr::null_mut();
        let rc = captrim_effects_to_filter_desc(input.as_ptr(), &mut out);
        if rc != 0 {
            captrim_string_free(out);
            return Err(rc);
        }
        let s = unsafe { cstr(out) }.unwrap().to_string();
        captrim_string_free(out);
        Ok(s)
    }

    #[test]
    fn effects_json_translates_in_canonical_order() {
        assert_eq!(
            translate(r#"["blur","invert","hflip"]"#).unwrap(),
            "hflip,negate,boxblur=2:1"
        );
        assert_eq!(translate("[]").unwrap(), "");
    }

    #[test]
    fn effects_json_rejects_junk_and_unknown_names() {
        assert!(translate(r#"["sepia"]"#).is_err());
        assert!(translate(r#"{"effect":"hflip"}"#).is_err());
        assert!(translate("not json").is_err());
    }
}
