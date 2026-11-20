//! Subtitle bitmap ABI.
//!
//! The frame plan says "this layer is a subtitle with clip_id X"; the renderer
//! needs pixels, not text, because it must not own font layout, wrapping or
//! outline maths. This module rasterises a clip's text into RGBA8 in the core
//! and hands the bytes over with the same buffer-retry protocol as
//! [`crate::captrim_engine_source_uri`].
//!
//! Two entry points share one implementation: the engine variant is what an
//! exporter uses (it writes the bitmap to a PNG that FFmpeg overlays), the
//! preview variant is what a compositor uses (it uploads the bitmap as a
//! texture). Same pixels either way -- that is the point.

use crate::{
    captrim_engine_t, captrim_preview_t, engine_from, ffi_guard, lock, preview::preview_from,
    Engine,
};
use captrim_core::text::{
    render_text, resolve_default_font, text_revision, FontSpec, TextBitmap, TextStyle,
};
use captrim_core::{Error, Result};
use std::os::raw::c_char;
use std::sync::Arc;

/// Cache key: clip, text/style revision, canvas size.
pub(crate) type TextCacheKey = (u64, u64, u32, u32);

pub(crate) fn font_spec(engine: &Engine) -> Result<FontSpec> {
    // Probed once per engine and remembered: `resolve_default_font` stats five
    // candidate paths, which is not something to repeat at 60 Hz.
    let mut slot = engine
        .text_font
        .lock()
        .map_err(|_| Error::Internal("font mutex poisoned".into()))?;
    if let Some(spec) = slot.clone() {
        return Ok(spec);
    }
    let spec = resolve_default_font()?;
    *slot = Some(spec.clone());
    Ok(spec)
}

fn cached(engine: &Engine, key: TextCacheKey) -> Option<Arc<TextBitmap>> {
    let cache = engine.text_cache.lock().ok()?;
    cache
        .iter()
        .find(|entry| entry.key == key)
        .map(|entry| entry.bitmap.clone())
}

fn remember(engine: &Engine, key: TextCacheKey, bitmap: Arc<TextBitmap>) {
    if let Ok(mut cache) = engine.text_cache.lock() {
        // Two subtitle tracks plus an edit in flight is already a busy frame;
        // eight entries covers the visible set with room for one stale copy.
        cache.retain(|entry| entry.key != key);
        cache.push(crate::TextCacheEntry { key, bitmap });
        let len = cache.len();
        if len > 8 {
            cache.drain(0..len - 8);
        }
    }
}

/// Rasterise a subtitle clip exactly as the preview would.
fn bitmap_for(
    engine: &Engine,
    clip_id: u64,
    canvas_w: u32,
    canvas_h: u32,
) -> Result<Arc<TextBitmap>> {
    if canvas_w == 0 || canvas_h == 0 {
        return Err(Error::invalid("canvas must be non-empty"));
    }
    let text = {
        let editor = lock(&engine.editor)?;
        editor
            .timeline()
            .find_clip(clip_id)
            .and_then(|(_, clip)| clip.text.clone())
            .ok_or_else(|| Error::not_found(format!("subtitle clip {clip_id}")))?
    };
    let style = TextStyle::subtitle(canvas_w, canvas_h);
    let revision = text_revision(&text, &style);
    let key: TextCacheKey = (clip_id, revision, canvas_w, canvas_h);
    if let Some(hit) = cached(engine, key) {
        return Ok(hit);
    }

    let spec = font_spec(engine)?;
    let bitmap = Arc::new(render_text(&text, &spec, &style)?);
    remember(engine, key, bitmap.clone());
    Ok(bitmap)
}

/// Write the bitmap into a caller buffer following the shared retry protocol.
///
/// `out_len` is the byte count of the RGBA8 pixels; the renderer multiplies
/// `out_width * out_height * 4` and must match it. A buffer that is too small
/// fails with `CAPTRIM_ERR_OUT_OF_RANGE` *after* the sizes are written, so a
/// caller can allocate exactly once and retry.
#[allow(clippy::too_many_arguments)]
fn copy_bitmap(
    bitmap: &TextBitmap,
    buf: *mut u8,
    buf_len: i32,
    out_len: *mut i32,
    out_width: *mut i32,
    out_height: *mut i32,
    out_revision: *mut u64,
) -> Result<()> {
    let needed = i32::try_from(bitmap.pixels.len())
        .map_err(|_| Error::OutOfRange("bitmap is too large for an i32 length".into()))?;
    if !out_len.is_null() {
        unsafe { *out_len = needed };
    }
    if !out_width.is_null() {
        unsafe { *out_width = bitmap.width as i32 };
    }
    if !out_height.is_null() {
        unsafe { *out_height = bitmap.height as i32 };
    }
    if !out_revision.is_null() {
        unsafe { *out_revision = bitmap.revision() };
    }
    if out_len.is_null() || out_width.is_null() || out_height.is_null() {
        return Err(Error::invalid(
            "out_len, out_width and out_height must not be null",
        ));
    }
    if buf.is_null() {
        // Query mode: sizes are already written, the caller allocates and calls
        // again. Not an error, but not a copy either.
        return Ok(());
    }
    if buf_len < needed {
        return Err(Error::OutOfRange(format!(
            "buffer holds {buf_len} bytes, bitmap needs {needed}"
        )));
    }
    // SAFETY: the caller promised buf_len writable bytes and we just checked the
    // bitmap fits; the source is a Rust slice that cannot overlap it.
    unsafe {
        std::ptr::copy_nonoverlapping(bitmap.pixels.as_ptr(), buf, bitmap.pixels.len());
    }
    Ok(())
}

/// RGBA8 bitmap of a subtitle clip at a given canvas size (engine flavour).
///
/// `buf == NULL` is a size query: the outputs are filled in and the call
/// succeeds without copying anything. Fonts come from the system unless
/// [`captrim_engine_set_text_font`] replaced them.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub extern "C" fn captrim_engine_clip_text_bitmap(
    engine: *mut captrim_engine_t,
    clip_id: u64,
    canvas_w: u32,
    canvas_h: u32,
    buf: *mut u8,
    buf_len: i32,
    out_len: *mut i32,
    out_width: *mut i32,
    out_height: *mut i32,
    out_revision: *mut u64,
) -> i32 {
    ffi_guard(|| {
        let e = engine_from(engine)?;
        let bitmap = bitmap_for(&e, clip_id, canvas_w, canvas_h)?;
        copy_bitmap(
            &bitmap,
            buf,
            buf_len,
            out_len,
            out_width,
            out_height,
            out_revision,
        )?;
        Ok(0)
    })
}

/// Same bitmap, addressed through a preview.
///
/// The compositor holds a preview handle, not an engine handle, so this is the
/// call its render thread makes once per subtitle layer per frame.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub extern "C" fn captrim_preview_clip_text_bitmap(
    preview: *mut captrim_preview_t,
    clip_id: u64,
    canvas_w: u32,
    canvas_h: u32,
    buf: *mut u8,
    buf_len: i32,
    out_len: *mut i32,
    out_width: *mut i32,
    out_height: *mut i32,
    out_revision: *mut u64,
) -> i32 {
    ffi_guard(|| {
        let p = preview_from(preview)?;
        let bitmap = bitmap_for(p.engine().as_ref(), clip_id, canvas_w, canvas_h)?;
        copy_bitmap(
            &bitmap,
            buf,
            buf_len,
            out_len,
            out_width,
            out_height,
            out_revision,
        )?;
        Ok(0)
    })
}

/// Replace the font used for subtitle rasterisation.
///
/// `path == NULL` or an empty string resets to "probe the system fonts". The
/// collection index selects a face inside a `.ttc`; pass 0 for plain `.ttf`.
/// Returns `CAPTRIM_ERR_IO` when the file cannot be read or parsed, so a host
/// that ships a custom font learns immediately instead of drawing nothing.
#[no_mangle]
pub extern "C" fn captrim_engine_set_text_font(
    engine: *mut captrim_engine_t,
    path: *const c_char,
    collection_index: u32,
) -> i32 {
    ffi_guard(|| {
        let e = engine_from(engine)?;
        if path.is_null() {
            *e.text_font
                .lock()
                .map_err(|_| Error::Internal("font mutex poisoned".into()))? = None;
            return Ok(0);
        }
        let path = unsafe { crate::cstr(path)? };
        if path.trim().is_empty() {
            *e.text_font
                .lock()
                .map_err(|_| Error::Internal("font mutex poisoned".into()))? = None;
            return Ok(0);
        }
        let spec = FontSpec {
            path: path.to_string(),
            collection_index,
        };
        // Parse once here so a bad path fails at the call site, with the
        // platform's own error handling, instead of inside the render loop.
        captrim_core::text::load_font(&spec)?;
        *e.text_font
            .lock()
            .map_err(|_| Error::Internal("font mutex poisoned".into()))? = Some(spec);
        if let Ok(mut cache) = e.text_cache.lock() {
            cache.clear();
        }
        Ok(0)
    })
}
