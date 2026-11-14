//! Stable C ABI for CapTrim.
//!
//! Design rules enforced here:
//! * no Rust value, enum or panic ever crosses the boundary;
//! * opaque handles are validated against a live registry, so a stale pointer
//!   yields `CAPTRIM_ERR_NOT_FOUND` instead of undefined behaviour;
//! * every entry point is wrapped in `catch_unwind`;
//! * all strings are plain UTF-8 + NUL, released by [`captrim_string_free`].

// `not_unsafe_ptr_arg_deref` assumes a Rust caller, where an `unsafe fn` is the
// correct way to flag pointer arguments. These functions are `extern "C"` entry
// points: the caller is JNI/NAPI/C, the safety contract lives in `captrim.h`,
// and marking them `unsafe fn` would only make every C-side declaration wrong
// without adding any check the code does not already perform. Every pointer is
// null-checked, and strings are validated as UTF-8 before use.
#![allow(clippy::not_unsafe_ptr_arg_deref)]

pub mod backend;
pub mod frame_desc;
pub mod preview;

pub use backend::{default_backend, MediaBackend, StubBackend};
// The preview entry points are `#[no_mangle] extern "C"`, and Rust callers of
// this crate expect to find them alongside the engine ones.
pub use frame_desc::*;
pub use preview::*;

use captrim_core::{
    code_message, Command, Editor, Error, MediaInfo, MediaSource, Result as CoreResult,
};
use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_float, c_void};
use std::ptr;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, MutexGuard, OnceLock};

/// Opaque token. The pointer value is a registry key; it is never dereferenced.
#[repr(C)]
pub struct captrim_engine_t {
    _private: [u8; 0],
}

pub type CaptrimProgressCb = Option<unsafe extern "C" fn(c_float, *mut c_void) -> i32>;

pub struct Engine {
    pub(crate) editor: Mutex<Editor>,
    backend: Arc<dyn MediaBackend>,
}

impl Engine {
    pub fn with_backend(editor: Editor, backend: Arc<dyn MediaBackend>) -> Self {
        Self {
            editor: Mutex::new(editor),
            backend,
        }
    }

    pub fn backend_name(&self) -> &'static str {
        self.backend.name()
    }
}

static ENGINES: OnceLock<Mutex<HashMap<u64, Arc<Engine>>>> = OnceLock::new();
static NEXT_HANDLE: AtomicU64 = AtomicU64::new(1);

fn registry() -> &'static Mutex<HashMap<u64, Arc<Engine>>> {
    ENGINES.get_or_init(|| Mutex::new(HashMap::new()))
}

pub(crate) fn lock<'a>(guard: &'a Mutex<Editor>) -> CoreResult<MutexGuard<'a, Editor>> {
    guard
        .lock()
        .map_err(|_| Error::Internal("editor mutex poisoned".into()))
}

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::new("").unwrap());
}

fn set_last_error(msg: &str) {
    let clean = msg.replace('\0', "");
    LAST_ERROR.with(|slot| {
        *slot.borrow_mut() = CString::new(clean).unwrap_or_else(|_| CString::new("").unwrap());
    });
}

pub(crate) unsafe fn cstr<'a>(p: *const c_char) -> CoreResult<&'a str> {
    if p.is_null() {
        return Err(Error::invalid("null string argument"));
    }
    CStr::from_ptr(p)
        .to_str()
        .map_err(|_| Error::invalid("argument is not valid UTF-8"))
}

pub(crate) fn owned_cstring(s: String) -> CoreResult<*mut c_char> {
    let clean = s.replace('\0', "");
    Ok(CString::new(clean)
        .map_err(|e| Error::Internal(e.to_string()))?
        .into_raw())
}

pub(crate) fn engine_from(h: *mut captrim_engine_t) -> CoreResult<Arc<Engine>> {
    if h.is_null() {
        return Err(Error::invalid("null engine handle"));
    }
    let key = h as usize as u64;
    registry()
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .get(&key)
        .cloned()
        .ok_or_else(|| Error::not_found("engine handle is invalid or already freed"))
}

pub(crate) fn ffi_guard<F>(f: F) -> i32
where
    F: FnOnce() -> CoreResult<i32>,
{
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)) {
        Ok(Ok(0)) => 0,
        Ok(Ok(code)) => {
            set_last_error(code_message(code));
            code
        }
        Ok(Err(err)) => {
            set_last_error(&err.to_string());
            err.code()
        }
        Err(_) => {
            set_last_error("panic caught at FFI boundary");
            Error::PANIC_CODE
        }
    }
}

static VERSION: &str = concat!("CapTrim ", env!("CARGO_PKG_VERSION"), "\0");

#[no_mangle]
pub extern "C" fn captrim_version() -> *const c_char {
    VERSION.as_ptr() as *const c_char
}

#[no_mangle]
pub extern "C" fn captrim_code_message(code: i32) -> *const c_char {
    set_last_error(code_message(code));
    LAST_ERROR.with(|slot| slot.borrow().as_ptr())
}

#[no_mangle]
pub extern "C" fn captrim_last_error_message() -> *const c_char {
    LAST_ERROR.with(|slot| slot.borrow().as_ptr())
}

#[no_mangle]
pub extern "C" fn captrim_string_free(s: *mut c_char) {
    if !s.is_null() {
        unsafe {
            drop(CString::from_raw(s));
        }
    }
}

#[no_mangle]
pub extern "C" fn captrim_engine_new(
    draft_json: *const c_char,
    out_engine: *mut *mut captrim_engine_t,
) -> i32 {
    ffi_guard(|| {
        if out_engine.is_null() {
            return Err(Error::invalid("out_engine is null"));
        }
        unsafe { *out_engine = ptr::null_mut() };

        let editor = if draft_json.is_null() {
            Editor::new()
        } else {
            let text = unsafe { cstr(draft_json)? };
            if text.trim().is_empty() {
                Editor::new()
            } else {
                Editor::from_draft(text)?
            }
        };

        let engine = Arc::new(Engine::with_backend(editor, default_backend()));
        let key = NEXT_HANDLE.fetch_add(1, Ordering::Relaxed);
        registry()
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .insert(key, engine);
        unsafe { *out_engine = key as usize as *mut captrim_engine_t };
        Ok(0)
    })
}

#[no_mangle]
pub extern "C" fn captrim_engine_free(engine: *mut captrim_engine_t) {
    if engine.is_null() {
        return;
    }
    let key = engine as usize as u64;
    let removed = registry()
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .remove(&key);
    drop(removed);
}

#[no_mangle]
pub extern "C" fn captrim_engine_apply(
    engine: *mut captrim_engine_t,
    command_json: *const c_char,
    created_id_out: *mut u64,
) -> i32 {
    ffi_guard(|| {
        let e = engine_from(engine)?;
        let text = unsafe { cstr(command_json)? };
        let mut editor = lock(&e.editor)?;
        let created = editor.apply_json(text)?;
        if !created_id_out.is_null() {
            unsafe { *created_id_out = created.unwrap_or(0) };
        }
        Ok(0)
    })
}

#[no_mangle]
pub extern "C" fn captrim_engine_undo(engine: *mut captrim_engine_t) -> i32 {
    ffi_guard(|| {
        let e = engine_from(engine)?;
        let mut editor = lock(&e.editor)?;
        if !editor.undo() {
            return Err(Error::OutOfRange("nothing to undo".into()));
        }
        Ok(0)
    })
}

#[no_mangle]
pub extern "C" fn captrim_engine_redo(engine: *mut captrim_engine_t) -> i32 {
    ffi_guard(|| {
        let e = engine_from(engine)?;
        let mut editor = lock(&e.editor)?;
        if !editor.redo() {
            return Err(Error::OutOfRange("nothing to redo".into()));
        }
        Ok(0)
    })
}

#[no_mangle]
pub extern "C" fn captrim_engine_draft(
    engine: *mut captrim_engine_t,
    out_json: *mut *mut c_char,
) -> i32 {
    ffi_guard(|| {
        if out_json.is_null() {
            return Err(Error::invalid("out_json is null"));
        }
        unsafe { *out_json = ptr::null_mut() };
        let e = engine_from(engine)?;
        let json = lock(&e.editor)?.draft_json()?;
        unsafe { *out_json = owned_cstring(json)? };
        Ok(0)
    })
}

#[no_mangle]
pub extern "C" fn captrim_engine_add_media(
    engine: *mut captrim_engine_t,
    uri: *const c_char,
    out_media_id: *mut u64,
) -> i32 {
    ffi_guard(|| {
        let e = engine_from(engine)?;
        let uri = unsafe { cstr(uri)? };
        let mut editor = lock(&e.editor)?;
        let created = editor.apply(&Command::AddMedia { uri: uri.into() })?;
        if !out_media_id.is_null() {
            unsafe { *out_media_id = created.unwrap_or(0) };
        }
        Ok(0)
    })
}

fn attach_media_info(
    e: &Engine,
    uri: Option<&str>,
    media_id: Option<u64>,
    info: MediaInfo,
) -> CoreResult<()> {
    let mut editor = lock(&e.editor)?;
    let mut timeline = editor.timeline().clone();
    let position = match (media_id, uri) {
        (Some(id), _) => timeline.media.iter().position(|m| m.id == id),
        (None, Some(u)) => timeline.media.iter().position(|m| m.uri == u),
        _ => None,
    };
    match position {
        Some(i) => timeline.media[i].info = Some(info),
        None => {
            let id = timeline.next_id();
            let uri = uri.unwrap_or("").to_string();
            timeline.media.push(MediaSource {
                id,
                uri,
                info: Some(info),
            });
        }
    }
    editor.set_timeline_silent(timeline)
}

#[no_mangle]
pub extern "C" fn captrim_engine_probe(
    engine: *mut captrim_engine_t,
    uri: *const c_char,
    out_json: *mut *mut c_char,
) -> i32 {
    ffi_guard(|| {
        if out_json.is_null() {
            return Err(Error::invalid("out_json is null"));
        }
        unsafe { *out_json = ptr::null_mut() };
        let e = engine_from(engine)?;
        let uri = unsafe { cstr(uri)? };
        let json = e.backend.probe(uri)?;
        let info: MediaInfo = serde_json::from_str(&json)
            .map_err(|err| Error::Internal(format!("native probe returned invalid JSON: {err}")))?;
        attach_media_info(&e, Some(uri), None, info)?;
        unsafe { *out_json = owned_cstring(json)? };
        Ok(0)
    })
}

#[no_mangle]
pub extern "C" fn captrim_engine_set_media_info(
    engine: *mut captrim_engine_t,
    media_id: u64,
    info_json: *const c_char,
) -> i32 {
    ffi_guard(|| {
        let e = engine_from(engine)?;
        let text = unsafe { cstr(info_json)? };
        let info: MediaInfo = serde_json::from_str(text)
            .map_err(|err| Error::invalid(format!("bad MediaInfo JSON: {err}")))?;
        attach_media_info(&e, None, Some(media_id), info)?;
        Ok(0)
    })
}

#[no_mangle]
pub extern "C" fn captrim_engine_export_remux(
    engine: *mut captrim_engine_t,
    src_uri: *const c_char,
    dst_uri: *const c_char,
    progress: CaptrimProgressCb,
    user_data: *mut c_void,
) -> i32 {
    ffi_guard(|| {
        let e = engine_from(engine)?;
        let src = unsafe { cstr(src_uri)? };
        let dst = unsafe { cstr(dst_uri)? };
        let mut report = |value: f32| match progress {
            Some(cb) => unsafe { cb(value, user_data) == 0 },
            None => true,
        };
        e.backend.remux(src, dst, &mut report)?;
        Ok(0)
    })
}

/// Copy a media source's URI into a caller buffer.
///
/// A renderer has a `source_id` from the frame plan and needs the file to open
/// a decoder; this is the lookup, kept inside the engine so the draft stays the
/// only place that knows where media lives. Passing too small a buffer fails
/// with `CAPTRIM_ERR_OUT_OF_RANGE` but still reports the required length, so a
/// caller can retry instead of guessing.
#[no_mangle]
pub extern "C" fn captrim_engine_source_uri(
    engine: *mut captrim_engine_t,
    source_id: u64,
    buf: *mut c_char,
    buf_len: i32,
    out_len: *mut i32,
) -> i32 {
    ffi_guard(|| {
        if buf.is_null() || out_len.is_null() {
            return Err(Error::invalid("buf and out_len must not be null"));
        }
        if buf_len <= 0 {
            return Err(Error::invalid("buf_len must be positive"));
        }
        let e = engine_from(engine)?;
        let uri = {
            let editor = lock(&e.editor)?;
            editor
                .timeline()
                .media
                .iter()
                .find(|m| m.id == source_id)
                .map(|m| m.uri.clone())
                .ok_or_else(|| Error::not_found(format!("media {source_id}")))?
        };

        let bytes = uri.as_bytes();
        let needed = i32::try_from(bytes.len())
            .map_err(|_| Error::OutOfRange("uri is too long for an i32 length".into()))?;
        // SAFETY: out_len is non-null and points at caller-owned storage.
        unsafe { *out_len = needed };
        if needed + 1 > buf_len {
            return Err(Error::OutOfRange(format!(
                "buffer holds {} bytes, uri needs {}",
                buf_len,
                needed + 1
            )));
        }
        // SAFETY: the caller promised buf_len writable bytes and we just proved
        // needed + 1 <= buf_len; source and destination cannot overlap.
        unsafe {
            ptr::copy_nonoverlapping(bytes.as_ptr(), buf as *mut u8, bytes.len());
            *buf.add(bytes.len()) = 0;
        }
        Ok(0)
    })
}
