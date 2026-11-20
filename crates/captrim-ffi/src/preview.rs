//! Preview ABI: frame planning plus the playback state machine.
//!
//! The platform owns the surface, the decoder and the GPU compositor; it does
//! *not* own the timing policy. Keeping the loop here means Android and
//! HarmonyOS cannot drift apart in their drop/duplicate behaviour, and it means
//! the preview reads the same plan the exporter will.

use crate::{captrim_engine_t, engine_from, ffi_guard, lock, owned_cstring, Engine};
use captrim_core::{
    plan_audio_at, plan_frame, plan_frame_at_index, Error, PreviewSession, Result as CoreResult,
};
use std::collections::HashMap;
use std::os::raw::{c_char, c_int};
use std::ptr;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, OnceLock};

/// Opaque token, same rules as [`captrim_engine_t`]: a registry key, never
/// dereferenced.
#[repr(C)]
pub struct captrim_preview_t {
    _private: [u8; 0],
}

pub(crate) struct PreviewState {
    pub(crate) session: PreviewSession,
    /// Editor revision the session was built from.
    revision: u64,
}

pub struct Preview {
    engine: Arc<Engine>,
    state: Mutex<PreviewState>,
}

impl Preview {
    /// The engine this preview renders from. Subtitle bitmaps are rasterised
    /// against the engine's timeline and font, so the compositor needs it.
    pub(crate) fn engine(&self) -> &Arc<Engine> {
        &self.engine
    }

    /// Pull the engine's timeline if it changed since the last frame.
    ///
    /// This is what makes an edit show up without the platform having to know
    /// that editing and rendering are coupled: the compositor gets a new plan
    /// for the same frame index as soon as the draft's revision moves.
    pub(crate) fn sync_from_engine(&self, state: &mut PreviewState) -> CoreResult<()> {
        let revision = lock(&self.engine.editor)?.revision();
        if revision == state.revision {
            return Ok(());
        }
        let timeline = lock(&self.engine.editor)?.timeline().clone();
        state.session.set_timeline(timeline);
        state.revision = revision;
        Ok(())
    }
}

static PREVIEWS: OnceLock<Mutex<HashMap<u64, Arc<Preview>>>> = OnceLock::new();
static NEXT_PREVIEW: AtomicU64 = AtomicU64::new(1);

fn registry() -> &'static Mutex<HashMap<u64, Arc<Preview>>> {
    PREVIEWS.get_or_init(|| Mutex::new(HashMap::new()))
}

pub(crate) fn preview_from(h: *mut captrim_preview_t) -> CoreResult<Arc<Preview>> {
    if h.is_null() {
        return Err(Error::invalid("null preview handle"));
    }
    let key = h as usize as u64;
    registry()
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .get(&key)
        .cloned()
        .ok_or_else(|| Error::not_found("preview handle is invalid or already freed"))
}

pub(crate) fn state(preview: &Preview) -> CoreResult<std::sync::MutexGuard<'_, PreviewState>> {
    preview
        .state
        .lock()
        .map_err(|_| Error::Internal("preview mutex poisoned".into()))
}

/// Plan one frame by index. This is what an exporter should call.
#[no_mangle]
pub extern "C" fn captrim_engine_plan_frame_at(
    engine: *mut captrim_engine_t,
    frame_index: u64,
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
            plan_frame_at_index(editor.timeline(), frame_index).to_json()?
        };
        unsafe { *out_json = owned_cstring(json)? };
        Ok(0)
    })
}

/// Plan the frame covering `t_us`. This is what a scrubber should call.
#[no_mangle]
pub extern "C" fn captrim_engine_plan_frame(
    engine: *mut captrim_engine_t,
    t_us: i64,
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
            plan_frame(editor.timeline(), t_us).to_json()?
        };
        unsafe { *out_json = owned_cstring(json)? };
        Ok(0)
    })
}

/// Plan the audio contributors at an arbitrary instant, without frame
/// quantisation. The audio renderer calls this at its callback rate, not at
/// the video frame rate, so feeding `t_us` through `plan_frame` would slide
/// every audio block onto the nearest frame boundary.
#[no_mangle]
pub extern "C" fn captrim_engine_plan_audio(
    engine: *mut captrim_engine_t,
    t_us: i64,
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
            serde_json::to_string(&plan_audio_at(editor.timeline(), t_us))
                .map_err(|e| Error::Internal(e.to_string()))?
        };
        unsafe { *out_json = owned_cstring(json)? };
        Ok(0)
    })
}

#[no_mangle]
pub extern "C" fn captrim_engine_duration(
    engine: *mut captrim_engine_t,
    out_duration_us: *mut i64,
    out_frame_count: *mut u64,
) -> i32 {
    ffi_guard(|| {
        let e = engine_from(engine)?;
        let editor = lock(&e.editor)?;
        let timeline = editor.timeline();
        if !out_duration_us.is_null() {
            unsafe { *out_duration_us = timeline.duration_us() };
        }
        if !out_frame_count.is_null() {
            unsafe { *out_frame_count = timeline.frame_count() };
        }
        Ok(0)
    })
}

#[no_mangle]
pub extern "C" fn captrim_preview_new(
    engine: *mut captrim_engine_t,
    out_preview: *mut *mut captrim_preview_t,
) -> i32 {
    ffi_guard(|| {
        if out_preview.is_null() {
            return Err(Error::invalid("out_preview is null"));
        }
        unsafe { *out_preview = ptr::null_mut() };
        let engine = engine_from(engine)?;
        let (revision, timeline) = {
            let editor = lock(&engine.editor)?;
            (editor.revision(), editor.timeline().clone())
        };
        let preview = Arc::new(Preview {
            engine,
            state: Mutex::new(PreviewState {
                session: PreviewSession::new(timeline),
                revision,
            }),
        });
        let key = NEXT_PREVIEW.fetch_add(1, Ordering::Relaxed);
        registry()
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .insert(key, preview);
        unsafe { *out_preview = key as usize as *mut captrim_preview_t };
        Ok(0)
    })
}

#[no_mangle]
pub extern "C" fn captrim_preview_free(preview: *mut captrim_preview_t) {
    if preview.is_null() {
        return;
    }
    let key = preview as usize as u64;
    let removed = registry()
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .remove(&key);
    drop(removed);
}

#[no_mangle]
pub extern "C" fn captrim_preview_seek(
    preview: *mut captrim_preview_t,
    t_us: i64,
    now_wall_us: i64,
) -> i32 {
    ffi_guard(|| {
        let p = preview_from(preview)?;
        let mut st = state(&p)?;
        p.sync_from_engine(&mut st)?;
        st.session.seek(t_us, now_wall_us);
        Ok(0)
    })
}

#[no_mangle]
pub extern "C" fn captrim_preview_set_playing(
    preview: *mut captrim_preview_t,
    playing: c_int,
    now_wall_us: i64,
) -> i32 {
    ffi_guard(|| {
        let p = preview_from(preview)?;
        let mut st = state(&p)?;
        p.sync_from_engine(&mut st)?;
        st.session.set_playing(playing != 0, now_wall_us);
        Ok(0)
    })
}

/// Returns the next [`captrim_core::Step`] as JSON:
/// `{"kind":"present","frame":{...}}`, `{"kind":"wait_frame","due_wall_us":N}`,
/// `{"kind":"idle"}` or `{"kind":"end","at_us":N}`.
#[no_mangle]
pub extern "C" fn captrim_preview_step(
    preview: *mut captrim_preview_t,
    now_wall_us: i64,
    out_json: *mut *mut c_char,
) -> i32 {
    ffi_guard(|| {
        if out_json.is_null() {
            return Err(Error::invalid("out_json is null"));
        }
        unsafe { *out_json = ptr::null_mut() };
        let p = preview_from(preview)?;
        let json = {
            let mut st = state(&p)?;
            p.sync_from_engine(&mut st)?;
            let step = st.session.step(now_wall_us);
            serde_json::to_string(&step).map_err(|e| Error::Internal(e.to_string()))?
        };
        unsafe { *out_json = owned_cstring(json)? };
        Ok(0)
    })
}

#[no_mangle]
pub extern "C" fn captrim_preview_position(
    preview: *mut captrim_preview_t,
    now_wall_us: i64,
    out_us: *mut i64,
) -> i32 {
    ffi_guard(|| {
        let p = preview_from(preview)?;
        let st = state(&p)?;
        if !out_us.is_null() {
            unsafe { *out_us = st.session.position_at(now_wall_us) };
        }
        Ok(0)
    })
}

/// Re-anchor to the audio renderer's position. `out_drift_us` reports what the
/// correction was, which is the single most useful number when debugging
/// "the preview looks slightly out of sync on this device".
#[no_mangle]
pub extern "C" fn captrim_preview_sync_audio(
    preview: *mut captrim_preview_t,
    audio_position_us: i64,
    now_wall_us: i64,
    out_drift_us: *mut i64,
) -> i32 {
    ffi_guard(|| {
        let p = preview_from(preview)?;
        let mut st = state(&p)?;
        let drift = st.session.sync_to_audio(audio_position_us, now_wall_us);
        if !out_drift_us.is_null() {
            unsafe { *out_drift_us = drift };
        }
        Ok(0)
    })
}

/// Dropped-frame count, for device profiling.
#[no_mangle]
pub extern "C" fn captrim_preview_stats(
    preview: *mut captrim_preview_t,
    out_dropped_frames: *mut u64,
) -> i32 {
    ffi_guard(|| {
        let p = preview_from(preview)?;
        let st = state(&p)?;
        if !out_dropped_frames.is_null() {
            unsafe { *out_dropped_frames = st.session.dropped_frames() };
        }
        Ok(0)
    })
}
