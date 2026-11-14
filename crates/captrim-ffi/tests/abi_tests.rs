//! Contract tests for the C ABI. These run on the host with no NDK and no
//! FFmpeg, which is exactly the configuration CI uses.

use captrim::*;
use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::ptr;

fn c(s: &str) -> CString {
    CString::new(s).unwrap()
}

unsafe fn take_string(p: *mut c_char) -> String {
    assert!(!p.is_null());
    let text = CStr::from_ptr(p).to_string_lossy().into_owned();
    captrim_string_free(p);
    text
}

unsafe fn last_error() -> String {
    CStr::from_ptr(captrim_last_error_message())
        .to_string_lossy()
        .into_owned()
}

struct TestEngine(*mut captrim_engine_t);

impl TestEngine {
    fn new() -> Self {
        let mut h: *mut captrim_engine_t = ptr::null_mut();
        let code = captrim_engine_new(ptr::null(), &mut h);
        assert_eq!(code, 0, "{}", unsafe { last_error() });
        assert!(!h.is_null());
        Self(h)
    }
    fn handle(&self) -> *mut captrim_engine_t {
        self.0
    }
}

impl Drop for TestEngine {
    fn drop(&mut self) {
        captrim_engine_free(self.0);
    }
}

#[test]
fn version_is_a_nul_terminated_static_string() {
    let v = unsafe { CStr::from_ptr(captrim_version()) };
    assert!(v.to_string_lossy().starts_with("CapTrim "));
}

#[test]
fn apply_command_and_draft_round_trip() {
    let e = TestEngine::new();
    let mut created: u64 = 0;
    let cmd = c(r#"{"type":"add_media","uri":"file:///a.mp4"}"#);
    assert_eq!(
        captrim_engine_apply(e.handle(), cmd.as_ptr(), &mut created),
        0
    );
    assert_eq!(created, 1);

    let track = c(r#"{"type":"add_track","kind":"main_video"}"#);
    assert_eq!(
        captrim_engine_apply(e.handle(), track.as_ptr(), &mut created),
        0
    );
    assert_eq!(created, 2);

    let clip = c(
        r#"{"type":"insert_clip","track_id":2,"clip":{"id":0,"source":1,"in_us":0,"out_us":3000000,"start_us":0,"speed":{"num":1,"den":1},"volume":1.0,"transform":{"scale":1.0,"dx":0.0,"dy":0.0,"rotation_deg":0.0},"effects":[]}}"#,
    );
    assert_eq!(
        captrim_engine_apply(e.handle(), clip.as_ptr(), &mut created),
        0,
        "{}",
        unsafe { last_error() }
    );
    assert_eq!(created, 3);

    let mut draft: *mut c_char = ptr::null_mut();
    assert_eq!(captrim_engine_draft(e.handle(), &mut draft), 0);
    let json = unsafe { take_string(draft) };
    assert!(json.contains("\"start_us\":0"), "{json}");
    assert!(json.contains("\"version\":1"), "{json}");

    assert_eq!(captrim_engine_undo(e.handle()), 0);
    let mut draft: *mut c_char = ptr::null_mut();
    assert_eq!(captrim_engine_draft(e.handle(), &mut draft), 0);
    let json = unsafe { take_string(draft) };
    assert!(
        !json.contains("\"source\":1"),
        "clip should be undone: {json}"
    );

    assert_eq!(captrim_engine_redo(e.handle()), 0);
    assert_eq!(captrim_engine_redo(e.handle()), -3);
}

#[test]
fn null_arguments_are_rejected_not_dereferenced() {
    let e = TestEngine::new();
    assert_eq!(
        captrim_engine_apply(e.handle(), ptr::null(), ptr::null_mut()),
        -1
    );
    assert!(unsafe { last_error() }.contains("null string"));
    assert_eq!(captrim_engine_new(ptr::null(), ptr::null_mut()), -1);
}

#[test]
fn freed_handle_is_detected() {
    let mut h: *mut captrim_engine_t = ptr::null_mut();
    assert_eq!(captrim_engine_new(ptr::null(), &mut h), 0);
    captrim_engine_free(h);
    let mut out: *mut c_char = ptr::null_mut();
    assert_eq!(captrim_engine_draft(h, &mut out), -2);
    captrim_engine_free(h); // double free must be a no-op
}

#[test]
fn bad_command_json_is_a_clean_error() {
    let e = TestEngine::new();
    let bad = c("not json");
    assert_eq!(
        captrim_engine_apply(e.handle(), bad.as_ptr(), ptr::null_mut()),
        -1
    );
    assert!(unsafe { last_error() }.contains("bad command JSON"));
    assert_eq!(
        unsafe { CStr::from_ptr(captrim_code_message(-4)) }.to_string_lossy(),
        "backend unavailable"
    );
}

#[test]
fn platform_side_probe_data_can_be_injected_without_a_backend() {
    let e = TestEngine::new();
    let mut media_id: u64 = 0;
    let uri = c("file:///a.mp4");
    assert_eq!(
        captrim_engine_add_media(e.handle(), uri.as_ptr(), &mut media_id),
        0
    );

    let info = c(
        r#"{"uri":"file:///a.mp4","format":"mov,mp4","duration_us":1000000,"bitrate_bps":1000,"streams":[]}"#,
    );
    assert_eq!(
        captrim_engine_set_media_info(e.handle(), media_id, info.as_ptr()),
        0
    );

    let mut draft: *mut c_char = ptr::null_mut();
    assert_eq!(captrim_engine_draft(e.handle(), &mut draft), 0);
    let json = unsafe { take_string(draft) };
    assert!(json.contains("\"format\":\"mov,mp4\""), "{json}");
}

#[cfg(not(feature = "ffmpeg"))]
#[test]
fn host_build_reports_missing_native_backend_explicitly() {
    let e = TestEngine::new();
    let uri = c("file:///a.mp4");
    let mut out: *mut c_char = ptr::null_mut();
    assert_eq!(captrim_engine_probe(e.handle(), uri.as_ptr(), &mut out), -4);
    assert!(unsafe { last_error() }.contains("ffmpeg"));

    let dst = c("file:///out.mp4");
    assert_eq!(
        captrim_engine_export_remux(
            e.handle(),
            uri.as_ptr(),
            dst.as_ptr(),
            None,
            ptr::null_mut()
        ),
        -4
    );
}
