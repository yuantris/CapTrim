//! Subtitle burn-in export.
//!
//! One call does the whole job: rasterise this timeline's subtitles into
//! `work_dir` (PAM, RGBA8), build the `[in] ... [out]` overlay graph around
//! the caller's own prep/post chains, and hand it to the same transcode
//! backend [`crate::captrim_engine_export_transcode`] uses. The host never
//! assembles filter strings, so it cannot drift from the preview's geometry.

use crate::text::font_spec;
use crate::{captrim_engine_t, engine_from, ffi_guard, lock};
use captrim_core::subtitles::{burn_filter, plan_burn};
use captrim_core::Error;
use std::os::raw::{c_char, c_void};
use std::path::Path;

/// Export `src_uri` to `dst_uri` with the timeline's subtitle clips burned in.
///
/// `filter_prefix` runs first, directly after `[in]`: it must trim the source
/// to the timeline's time range, put the picture on the timeline canvas
/// (`timeline.width` × `timeline.height`) and reset PTS to zero
/// (`setpts=PTS-STARTPTS`) — the subtitle windows are timeline microseconds
/// measured from that zero. `filter_suffix` runs after the last overlay (downscale, `fps`,
/// effects). Both may be NULL or "".
///
/// `work_dir` receives one `sub_<clip_id>.pam` per subtitle and must be a
/// directory whose absolute path contains no `:`, quote, comma, semicolon,
/// bracket or `%` — FFmpeg filter graphs cannot carry those in a path. That
/// makes this a device-side call by design (Windows paths carry a drive colon).
///
/// Returns the backend's status code; `CAPTRIM_ERR_NO_BACKEND` when built
/// without the `ffmpeg` feature, `CAPTRIM_ERR_INVALID` for an unusable
/// directory or an empty canvas.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub extern "C" fn captrim_engine_export_burn_subtitles(
    engine: *mut captrim_engine_t,
    src_uri: *const c_char,
    dst_uri: *const c_char,
    filter_prefix: *const c_char,
    filter_suffix: *const c_char,
    video_codec: *const c_char,
    work_dir: *const c_char,
    progress: crate::CaptrimProgressCb,
    user_data: *mut c_void,
) -> i32 {
    ffi_guard(|| {
        let e = engine_from(engine)?;
        let src = unsafe { crate::cstr(src_uri)? };
        let dst = unsafe { crate::cstr(dst_uri)? };
        let prefix = if filter_prefix.is_null() {
            ""
        } else {
            unsafe { crate::cstr(filter_prefix)? }
        };
        let suffix = if filter_suffix.is_null() {
            ""
        } else {
            unsafe { crate::cstr(filter_suffix)? }
        };
        let codec = if video_codec.is_null() {
            ""
        } else {
            unsafe { crate::cstr(video_codec)? }
        };
        if work_dir.is_null() {
            return Err(Error::invalid("work_dir must not be null"));
        }
        let work_dir = unsafe { crate::cstr(work_dir)? };
        // Checked before anything else that can fail for an unrelated reason:
        // on a host without a system font the font lookup would answer first and
        // turn "your directory is unusable" into "no font found".
        captrim_core::subtitles::validate_burn_dir(work_dir)?;

        // Clone the timeline so the (possibly long) rasterise + transcode never
        // holds the editor lock: the UI must stay able to edit while exporting.
        let timeline = {
            let editor = lock(&e.editor)?;
            editor.timeline().clone()
        };
        let font = font_spec(&e)?;
        let layers = plan_burn(&timeline, Path::new(work_dir), &font)?;
        let graph = burn_filter(&layers, prefix, suffix)?;

        let mut report = |value: f32| match progress {
            Some(cb) => unsafe { cb(value, user_data) == 0 },
            None => true,
        };
        e.backend.transcode(src, dst, &graph, codec, &mut report)?;
        Ok(0)
    })
}
