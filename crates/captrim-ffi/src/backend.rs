//! Media backend seam.
//!
//! The host build (no NDK, no FFmpeg) uses `StubBackend`, so `cargo test`
//! works everywhere. Mobile builds enable the `ffmpeg` feature and link the
//! C shim in `native/`, which is the only component that touches FFmpeg.

use captrim_core::{Error, Result};
use std::sync::Arc;

pub trait MediaBackend: Send + Sync {
    fn name(&self) -> &'static str;
    /// Returns a JSON `MediaInfo`.
    fn probe(&self, uri: &str) -> Result<String>;
    /// Lossless stream copy. `progress` returns false to cancel.
    fn remux(
        &self,
        src_uri: &str,
        dst_uri: &str,
        progress: &mut dyn FnMut(f32) -> bool,
    ) -> Result<()>;
    /// Decode -> filter graph -> re-encode -> mux. `filter_desc` is a
    /// libavfilter graph string (e.g. "scale=640:-2,fps=24"); `video_codec` is
    /// an encoder name (e.g. "libx264"); empty strings use backend defaults.
    /// This is the software backbone and the fallback for hardware codecs.
    /// `progress` returns false to cancel.
    fn transcode(
        &self,
        src_uri: &str,
        dst_uri: &str,
        filter_desc: &str,
        video_codec: &str,
        progress: &mut dyn FnMut(f32) -> bool,
    ) -> Result<()>;
    /// Run one compiled timeline graph: N inputs, one `filter_complex`, its
    /// `vout` (and optional `aout`) encoded into one mp4. Unlike `transcode`
    /// this is the whole edited timeline, not one file. `progress` returns
    /// false to cancel.
    fn export_graph(
        &self,
        graph: &captrim_core::ExportGraph,
        dst_uri: &str,
        progress: &mut dyn FnMut(f32) -> bool,
    ) -> Result<()>;
    /// Whether this FFmpeg build can encode with `encoder` (a libavcodec
    /// encoder name such as `libx264`). The answer is per-build, not per-device:
    /// the LGPL profile simply does not contain the GPL encoders.
    fn encoder_available(&self, encoder: &str) -> bool;
    /// Whether this FFmpeg build contains the named libavfilter filter.
    ///
    /// Two filters the effect vocabulary needs are GPL inside FFmpeg itself:
    /// `eq` (brightness / contrast / saturation) and `boxblur` (blur). An LGPL
    /// build therefore has no colour grading and no blur, even though every
    /// other filter the compiler emits is present. The stub backend answers
    /// `true` — it cannot render anything, and its callers are type checks.
    fn filter_available(&self, _filter: &str) -> bool {
        true
    }
}

pub struct StubBackend;

impl MediaBackend for StubBackend {
    fn name(&self) -> &'static str {
        "stub"
    }

    fn probe(&self, _uri: &str) -> Result<String> {
        Err(Error::BackendUnavailable(
            "build captrim-ffi with --features ffmpeg and ship libcaptrim_ffmpeg.so".into(),
        ))
    }

    fn remux(
        &self,
        _src_uri: &str,
        _dst_uri: &str,
        _progress: &mut dyn FnMut(f32) -> bool,
    ) -> Result<()> {
        Err(Error::BackendUnavailable(
            "build captrim-ffi with --features ffmpeg and ship libcaptrim_ffmpeg.so".into(),
        ))
    }

    fn transcode(
        &self,
        _src_uri: &str,
        _dst_uri: &str,
        _filter_desc: &str,
        _video_codec: &str,
        _progress: &mut dyn FnMut(f32) -> bool,
    ) -> Result<()> {
        Err(Error::BackendUnavailable(
            "build captrim-ffi with --features ffmpeg and ship libcaptrim_ffmpeg.so".into(),
        ))
    }

    fn export_graph(
        &self,
        _graph: &captrim_core::ExportGraph,
        _dst_uri: &str,
        _progress: &mut dyn FnMut(f32) -> bool,
    ) -> Result<()> {
        Err(Error::BackendUnavailable(
            "build captrim-ffi with --features ffmpeg and ship libcaptrim_ffmpeg.so".into(),
        ))
    }

    fn encoder_available(&self, _encoder: &str) -> bool {
        false
    }
}

pub fn default_backend() -> Arc<dyn MediaBackend> {
    #[cfg(feature = "ffmpeg")]
    {
        Arc::new(ffmpeg::FfmpegBackend)
    }
    #[cfg(not(feature = "ffmpeg"))]
    {
        Arc::new(StubBackend)
    }
}

#[cfg(feature = "ffmpeg")]
mod ffmpeg {
    use super::MediaBackend;
    use captrim_core::{Error, Result};
    use std::ffi::{CStr, CString};
    use std::os::raw::{c_char, c_float, c_int, c_void};
    use std::ptr;

    pub struct FfmpegBackend;

    #[link(name = "captrim_ffmpeg")]
    extern "C" {
        fn ct_ff_probe(
            uri: *const c_char,
            out_json: *mut *mut c_char,
            out_error: *mut *mut c_char,
        ) -> c_int;
        fn ct_ff_remux(
            src_uri: *const c_char,
            dst_uri: *const c_char,
            progress: Option<unsafe extern "C" fn(c_float, *mut c_void) -> c_int>,
            user_data: *mut c_void,
            out_error: *mut *mut c_char,
        ) -> c_int;
        fn ct_ff_transcode(
            src_uri: *const c_char,
            dst_uri: *const c_char,
            filter_desc: *const c_char,
            video_codec: *const c_char,
            progress: Option<unsafe extern "C" fn(c_float, *mut c_void) -> c_int>,
            user_data: *mut c_void,
            out_error: *mut *mut c_char,
        ) -> c_int;
        fn ct_ff_string_free(s: *mut c_char);
        fn ct_ff_encoder_available(name: *const c_char) -> c_int;
        fn ct_ff_filter_available(name: *const c_char) -> c_int;
        fn ct_ff_export_graph(
            src_uris: *const *const c_char,
            src_count: c_int,
            filter_complex: *const c_char,
            dst_uri: *const c_char,
            video_codec: *const c_char,
            crf: c_int,
            preset: *const c_char,
            audio_bitrate_kbps: c_int,
            duration_us: i64,
            progress: Option<unsafe extern "C" fn(c_float, *mut c_void) -> c_int>,
            user_data: *mut c_void,
            out_error: *mut *mut c_char,
        ) -> c_int;
    }

    unsafe extern "C" fn progress_trampoline(progress: c_float, user: *mut c_void) -> c_int {
        let closure = &mut *(user as *mut &mut dyn FnMut(f32) -> bool);
        if closure(progress) {
            0
        } else {
            1
        }
    }

    fn cs(s: &str) -> Result<CString> {
        CString::new(s).map_err(|_| Error::invalid("string contains an interior NUL byte"))
    }

    unsafe fn take_error(raw: *mut c_char) -> String {
        if raw.is_null() {
            return "unknown native backend error".into();
        }
        let text = CStr::from_ptr(raw).to_string_lossy().into_owned();
        ct_ff_string_free(raw);
        text
    }

    fn map_status(status: c_int, message: String) -> Error {
        match status {
            1 => Error::InvalidArgument(message),
            2 => Error::Io(message),
            3 => Error::Decode(message),
            4 => Error::Encode(message),
            5 => Error::Cancelled,
            _ => Error::Internal(message),
        }
    }

    impl MediaBackend for FfmpegBackend {
        fn name(&self) -> &'static str {
            "ffmpeg"
        }

        fn probe(&self, uri: &str) -> Result<String> {
            let uri = cs(uri)?;
            let mut json: *mut c_char = ptr::null_mut();
            let mut err: *mut c_char = ptr::null_mut();
            let status = unsafe { ct_ff_probe(uri.as_ptr(), &mut json, &mut err) };
            if status != 0 {
                return Err(map_status(status, unsafe { take_error(err) }));
            }
            if json.is_null() {
                return Err(Error::Internal("native probe returned no data".into()));
            }
            let text = unsafe { CStr::from_ptr(json) }
                .to_string_lossy()
                .into_owned();
            unsafe { ct_ff_string_free(json) };
            Ok(text)
        }

        fn remux(
            &self,
            src_uri: &str,
            dst_uri: &str,
            mut progress: &mut dyn FnMut(f32) -> bool,
        ) -> Result<()> {
            let src = cs(src_uri)?;
            let dst = cs(dst_uri)?;
            let mut err: *mut c_char = ptr::null_mut();
            let user = &mut progress as *mut _ as *mut c_void;
            let status = unsafe {
                ct_ff_remux(
                    src.as_ptr(),
                    dst.as_ptr(),
                    Some(progress_trampoline),
                    user,
                    &mut err,
                )
            };
            let message = unsafe { take_error(err) };
            if status != 0 {
                return Err(map_status(status, message));
            }
            Ok(())
        }

        fn transcode(
            &self,
            src_uri: &str,
            dst_uri: &str,
            filter_desc: &str,
            video_codec: &str,
            mut progress: &mut dyn FnMut(f32) -> bool,
        ) -> Result<()> {
            let src = cs(src_uri)?;
            let dst = cs(dst_uri)?;
            let filter = cs(filter_desc)?;
            let codec = cs(video_codec)?;
            let mut err: *mut c_char = ptr::null_mut();
            let user = &mut progress as *mut _ as *mut c_void;
            let status = unsafe {
                ct_ff_transcode(
                    src.as_ptr(),
                    dst.as_ptr(),
                    filter.as_ptr(),
                    codec.as_ptr(),
                    Some(progress_trampoline),
                    user,
                    &mut err,
                )
            };
            let message = unsafe { take_error(err) };
            if status != 0 {
                return Err(map_status(status, message));
            }
            Ok(())
        }

        fn export_graph(
            &self,
            graph: &captrim_core::ExportGraph,
            dst_uri: &str,
            mut progress: &mut dyn FnMut(f32) -> bool,
        ) -> Result<()> {
            // The C side owns the pointer list, so every CString has to outlive
            // the call: build them all first, then take the raw pointers.
            let dst = cs(dst_uri)?;
            let filter = cs(&graph.filter_complex)?;
            let codec = cs(&graph.codec)?;
            let preset = cs(&graph.preset)?;
            let mut inputs = Vec::with_capacity(graph.inputs.len());
            for uri in &graph.inputs {
                inputs.push(cs(uri)?);
            }
            let raw: Vec<*const c_char> = inputs.iter().map(|c| c.as_ptr()).collect();
            let mut err: *mut c_char = ptr::null_mut();
            let user = &mut progress as *mut _ as *mut c_void;
            let status = unsafe {
                ct_ff_export_graph(
                    raw.as_ptr(),
                    raw.len() as c_int,
                    filter.as_ptr(),
                    dst.as_ptr(),
                    codec.as_ptr(),
                    graph.crf as c_int,
                    preset.as_ptr(),
                    graph.audio_bitrate_kbps as c_int,
                    graph.duration_us,
                    Some(progress_trampoline),
                    user,
                    &mut err,
                )
            };
            let message = unsafe { take_error(err) };
            if status != 0 {
                return Err(map_status(status, message));
            }
            Ok(())
        }

        fn encoder_available(&self, encoder: &str) -> bool {
            // The C shim answers from avcodec_find_encoder_by_name, so this is
            // the truth of what the shipped FFmpeg actually contains, not a
            // guess from the licence profile.
            match cs(encoder) {
                Ok(name) => unsafe { ct_ff_encoder_available(name.as_ptr()) == 1 },
                Err(_) => false,
            }
        }

        fn filter_available(&self, filter: &str) -> bool {
            // Same reasoning as encoder_available: ask libavfilter, do not infer
            // from the profile. `eq` / `boxblur` are compiled out of an LGPL
            // build by FFmpeg's own configure (they are GPL filters).
            match cs(filter) {
                Ok(name) => unsafe { ct_ff_filter_available(name.as_ptr()) == 1 },
                Err(_) => false,
            }
        }
    }
}
