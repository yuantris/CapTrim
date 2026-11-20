//! Image and sticker bitmap ABI.
//!
//! The frame plan says "this layer is an image with clip_id X"; the pixels are
//! decoded, fitted, scaled and rotated in the core (see `captrim_core::images`)
//! so the preview and the exporter burn the same bitmap. This module adds the
//! caching layer: a compositor asks once per layer per frame, and only a
//! revision change costs a decode.
//!
//! Two entry points share one implementation: the engine flavour is what the
//! burn export uses, the preview flavour is what the GL compositor calls from
//! its render thread (it holds a preview handle, not an engine handle).

use crate::text::copy_bitmap;
use crate::{
    captrim_engine_t, captrim_preview_t, engine_from, ffi_guard, lock, preview::preview_from,
    Engine,
};
use captrim_core::images::{image_revision, render_image, ImageBitmap};
use captrim_core::{Error, Result};
use std::sync::Arc;

/// Cache key: clip, revision, canvas size.
pub(crate) type ImageCacheKey = (u64, u64, u32, u32);

fn cached(engine: &Engine, key: ImageCacheKey) -> Option<Arc<ImageBitmap>> {
    let cache = engine.image_cache.lock().ok()?;
    cache
        .iter()
        .find(|entry| entry.key == key)
        .map(|entry| entry.bitmap.clone())
}

fn remember(engine: &Engine, key: ImageCacheKey, bitmap: Arc<ImageBitmap>) {
    if let Ok(mut cache) = engine.image_cache.lock() {
        // Stickers are few and long-lived; four entries cover a busy frame
        // without letting a scrub through a sticker collage grow unbounded.
        cache.retain(|entry| entry.key != key);
        cache.push(crate::ImageCacheEntry { key, bitmap });
        let len = cache.len();
        if len > 4 {
            cache.drain(0..len - 4);
        }
    }
}

/// Render a clip's still exactly as the preview would.
pub(crate) fn bitmap_for(
    engine: &Engine,
    clip_id: u64,
    canvas_w: u32,
    canvas_h: u32,
) -> Result<Arc<ImageBitmap>> {
    if canvas_w == 0 || canvas_h == 0 {
        return Err(Error::invalid("canvas must be non-empty"));
    }
    let (uri, transform) = {
        let editor = lock(&engine.editor)?;
        let timeline = editor.timeline();
        let (_, clip) = timeline
            .find_clip(clip_id)
            .ok_or_else(|| Error::not_found(format!("image clip {clip_id}")))?;
        let asset = clip
            .image
            .as_ref()
            .ok_or_else(|| Error::not_found(format!("clip {clip_id} is not an image")))?;
        (asset.uri.clone(), clip.transform.clone())
    };
    let revision = image_revision(&uri, canvas_w, canvas_h, &transform);
    let key: ImageCacheKey = (clip_id, revision, canvas_w, canvas_h);
    if let Some(hit) = cached(engine, key) {
        return Ok(hit);
    }

    let source = captrim_core::images::load_image(&uri)?;
    let bitmap = Arc::new(render_image(&source, canvas_w, canvas_h, &transform, &uri)?);
    remember(engine, key, bitmap.clone());
    Ok(bitmap)
}

/// RGBA8 bitmap of an image clip at a given canvas size (engine flavour).
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub extern "C" fn captrim_engine_clip_image_bitmap(
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
            bitmap.width,
            bitmap.height,
            &bitmap.pixels,
            bitmap.revision,
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
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub extern "C" fn captrim_preview_clip_image_bitmap(
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
            bitmap.width,
            bitmap.height,
            &bitmap.pixels,
            bitmap.revision,
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
