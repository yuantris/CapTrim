//! Contract tests for the P1 preview ABI.
//!
//! These exercise the real `extern "C"` entry points, not the core types, so a
//! mistake in the FFI layer (wrong out-parameter, handle confusion, leaked
//! string) fails here instead of on a device.

use captrim::*;
use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::ptr;

fn c(s: &str) -> CString {
    CString::new(s).unwrap()
}

unsafe fn take_string(p: *mut c_char) -> String {
    assert!(!p.is_null(), "expected a string, got NULL");
    let text = CStr::from_ptr(p).to_string_lossy().into_owned();
    captrim_string_free(p);
    text
}

unsafe fn last_error() -> String {
    CStr::from_ptr(captrim_last_error_message())
        .to_string_lossy()
        .into_owned()
}

/// Engine + one 2 s clip on a 30 fps timeline: 60 frames.
struct Fixture {
    engine: *mut captrim_engine_t,
    preview: *mut captrim_preview_t,
}

impl Fixture {
    fn new() -> Self {
        let mut engine = ptr::null_mut();
        assert_eq!(captrim_engine_new(ptr::null(), &mut engine), 0);

        // 30 fps is the default, but set it explicitly so the numbers below
        // are not silently coupled to a default value.
        let canvas = c(
            r#"{"type":"set_canvas","width":1920,"height":1080,"frame_rate":{"num":30,"den":1}}"#,
        );
        assert_eq!(
            captrim_engine_apply(engine, canvas.as_ptr(), ptr::null_mut()),
            0,
            "{}",
            unsafe { last_error() }
        );
        Self::apply(engine, r#"{"type":"add_media","uri":"file:///a.mp4"}"#);
        Self::apply(engine, r#"{"type":"add_track","kind":"main_video"}"#);
        Self::apply(
            engine,
            r#"{"type":"insert_clip","track_id":2,"clip":{"id":0,"source":1,"in_us":0,"out_us":2000000,"start_us":0,"speed":{"num":1,"den":1},"volume":1.0,"transform":{"scale":1.0,"dx":0.0,"dy":0.0,"rotation_deg":0.0},"effects":[]}}"#,
        );

        let mut preview = ptr::null_mut();
        assert_eq!(captrim_preview_new(engine, &mut preview), 0);
        assert!(!preview.is_null());
        Self { engine, preview }
    }

    fn apply(engine: *mut captrim_engine_t, json: &str) {
        let cmd = c(json);
        assert_eq!(
            captrim_engine_apply(engine, cmd.as_ptr(), ptr::null_mut()),
            0,
            "{}",
            unsafe { last_error() }
        );
    }

    fn step(&self, now_us: i64) -> String {
        let mut out: *mut c_char = ptr::null_mut();
        assert_eq!(
            captrim_preview_step(self.preview, now_us, &mut out),
            0,
            "{}",
            unsafe { last_error() }
        );
        unsafe { take_string(out) }
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        captrim_preview_free(self.preview);
        captrim_engine_free(self.engine);
    }
}

#[test]
fn duration_reports_length_and_frame_count() {
    let f = Fixture::new();
    let mut duration_us: i64 = -1;
    let mut frames: u64 = 0;
    assert_eq!(
        captrim_engine_duration(f.engine, &mut duration_us, &mut frames),
        0
    );
    assert_eq!(duration_us, 2_000_000);
    assert_eq!(frames, 60);
}

#[test]
fn plan_frame_at_returns_an_exact_frame() {
    let f = Fixture::new();
    let mut out: *mut c_char = ptr::null_mut();
    assert_eq!(
        captrim_engine_plan_frame_at(f.engine, 30, &mut out),
        0,
        "{}",
        unsafe { last_error() }
    );
    let json = unsafe { take_string(out) };
    assert!(json.contains("\"frame_index\":30"), "{json}");
    assert!(json.contains("\"t_us\":1000000"), "{json}");
    // Frame 30 at 30 fps is exactly 1 s into the source.
    assert!(json.contains("\"source_us\":1000000"), "{json}");
}

#[test]
fn plan_frame_snaps_a_timestamp_onto_the_grid() {
    let f = Fixture::new();
    let mut out: *mut c_char = ptr::null_mut();
    assert_eq!(captrim_engine_plan_frame(f.engine, 1_500_000, &mut out), 0);
    let json = unsafe { take_string(out) };
    assert!(json.contains("\"frame_index\":45"), "{json}");
    assert!(json.contains("\"t_us\":1500000"), "{json}");
}

#[test]
fn plan_frame_rejects_a_null_out_pointer() {
    let f = Fixture::new();
    assert_eq!(captrim_engine_plan_frame(f.engine, 0, ptr::null_mut()), -1);
    assert!(unsafe { last_error() }.contains("out_json"));
}

#[test]
fn first_step_presents_the_first_frame() {
    let f = Fixture::new();
    let json = f.step(0);
    assert!(json.contains("\"kind\":\"present\""), "{json}");
    assert!(json.contains("\"frame_index\":0"), "{json}");
    assert!(json.contains("file:///a.mp4"), "{json}");
}

#[test]
fn paused_session_goes_idle() {
    let f = Fixture::new();
    f.step(0);
    assert_eq!(f.step(1_000_000), r#"{"kind":"idle"}"#);
}

#[test]
fn playing_session_waits_then_presents_the_next_frame() {
    let f = Fixture::new();
    assert_eq!(captrim_preview_set_playing(f.preview, 1, 0), 0);

    let first = f.step(0);
    assert!(first.contains("\"frame_index\":0"), "{first}");

    let second = f.step(10_000);
    assert!(second.contains("\"kind\":\"wait_frame\""), "{second}");
    assert!(second.contains("\"due_wall_us\":33333"), "{second}");

    let third = f.step(40_000);
    assert!(third.contains("\"frame_index\":1"), "{third}");
}

#[test]
fn playback_ends_at_the_duration() {
    let f = Fixture::new();
    assert_eq!(captrim_preview_set_playing(f.preview, 1, 0), 0);
    f.step(0);
    let json = f.step(5_000_000);
    assert!(json.contains("\"kind\":\"end\""), "{json}");
    assert!(json.contains("\"at_us\":2000000"), "{json}");
}

#[test]
fn seek_moves_the_playhead_and_invalidates_the_frame() {
    let f = Fixture::new();
    f.step(0);
    assert_eq!(captrim_preview_seek(f.preview, 1_000_000, 0), 0);
    let json = f.step(0);
    assert!(json.contains("\"frame_index\":30"), "{json}");
}

#[test]
fn position_reports_the_clock() {
    let f = Fixture::new();
    assert_eq!(captrim_preview_set_playing(f.preview, 1, 0), 0);
    let mut position: i64 = -1;
    assert_eq!(
        captrim_preview_position(f.preview, 500_000, &mut position),
        0
    );
    assert_eq!(position, 500_000);
}

#[test]
fn sync_audio_reports_the_drift_it_corrected() {
    let f = Fixture::new();
    assert_eq!(captrim_preview_set_playing(f.preview, 1, 0), 0);
    let mut drift: i64 = 0;
    // At wall time 0 the video clock reads 0; audio is already 600 ms in, so
    // audio is ahead and the drift is positive.
    assert_eq!(
        captrim_preview_sync_audio(f.preview, 600_000, 0, &mut drift),
        0
    );
    assert_eq!(drift, 600_000, "audio ahead by 600 ms");
}

#[test]
fn stats_count_dropped_frames() {
    let f = Fixture::new();
    assert_eq!(captrim_preview_set_playing(f.preview, 1, 0), 0);
    f.step(0);
    f.step(200_000); // stalls: 6 frames elapse, 5 are never drawn

    let mut dropped: u64 = 0;
    assert_eq!(captrim_preview_stats(f.preview, &mut dropped), 0);
    assert_eq!(dropped, 5);
}

#[test]
fn editing_the_draft_repaints_the_same_frame() {
    let f = Fixture::new();

    // Park on frame 30 (1 s) and show it once.
    assert_eq!(captrim_preview_seek(f.preview, 1_000_000, 0), 0);
    let before = f.step(0);
    assert!(before.contains("\"frame_index\":30"), "{before}");
    assert!(before.contains("file:///a.mp4"), "{before}");

    // Trim the clip to half a second, so frame 30 becomes a gap.
    Fixture::apply(
        f.engine,
        r#"{"type":"set_out_point","clip_id":3,"out_us":500000}"#,
    );

    // The platform tells the preview nothing: the session notices the revision
    // change on its next step, which is what makes an edit appear while the
    // playhead is parked.
    let after = f.step(0);
    assert!(after.contains("\"kind\":\"present\""), "{after}");
    assert!(after.contains("\"frame_index\":30"), "{after}");
    assert!(
        after.contains("\"layers\":[]"),
        "edit not picked up: {after}"
    );
}

#[test]
fn preview_of_a_freed_engine_is_still_bounded() {
    let mut engine = ptr::null_mut();
    assert_eq!(captrim_engine_new(ptr::null(), &mut engine), 0);
    let mut preview = ptr::null_mut();
    assert_eq!(captrim_preview_new(engine, &mut preview), 0);

    // Freeing the engine while a preview holds it must not crash: the preview
    // owns its own Arc, and the registry entry is just a key.
    captrim_engine_free(engine);

    let mut out: *mut c_char = ptr::null_mut();
    assert_eq!(captrim_preview_step(preview, 0, &mut out), 0);
    captrim_string_free(out);
    captrim_preview_free(preview);
}

#[test]
fn freed_preview_handle_is_detected() {
    let mut engine = ptr::null_mut();
    assert_eq!(captrim_engine_new(ptr::null(), &mut engine), 0);
    let mut preview = ptr::null_mut();
    assert_eq!(captrim_preview_new(engine, &mut preview), 0);
    captrim_preview_free(preview);

    let mut out: *mut c_char = ptr::null_mut();
    assert_eq!(captrim_preview_step(preview, 0, &mut out), -2);
    captrim_preview_free(preview); // double free must be a no-op
    captrim_engine_free(engine);
}

#[test]
fn null_preview_handle_is_rejected() {
    assert_eq!(captrim_preview_seek(ptr::null_mut(), 0, 0), -1);
    assert!(unsafe { last_error() }.contains("null preview"));
}
