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
        fn ct_ff_string_free(s: *mut c_char);
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
    }
}
