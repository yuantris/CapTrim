//! Image and sticker layers: decode a still and resample it to the exact pixels
//! both halves of the pipeline consume.
//!
//! The preview uploads this RGBA8 block as a plain `GL_TEXTURE_2D`; the exporter
//! writes the same block as a PAM and overlays it. There is deliberately no
//! "native" size handoff to either of them: scale, rotation and the fit-inside-
//! the-canvas rule are applied *here*, once, because the GL compositor and
//! FFmpeg `overlay` would otherwise disagree the first time someone rotates a
//! sticker (`overlay` cannot rotate at all).
//!
//! Decoding is pure Rust (`png` / `zune-jpeg` through the `image` crate). It
//! must not go through FFmpeg: HarmonyOS ships no `libz.so`, so FFmpeg's PNG
//! decoder is unavailable on the device that matters.
//!
//! Geometry, in the order the compositor's `Transform` documents it:
//!  1. fit: `min(canvas_w / src_w, canvas_h / src_h)` -- "as large as it goes
//!     without cropping";
//!  2. scale: multiplies the fitted size;
//!  3. rotation: clockwise on screen, expands the bitmap to its bounding box;
//!  4. dx / dy: *not* baked in -- the preview and the overlay both translate the
//!     bitmap's centre by fractions of the canvas, so they only have to agree on
//!     one formula (see [`crate::subtitles::plan_burn`]).

use crate::error::{Error, Result};
use crate::model::Transform;
use std::sync::Arc;

/// One decoded still, straight-alpha RGBA8, top row first.
#[derive(Debug)]
pub struct SourceImage {
    pub width: u32,
    pub height: u32,
    pub pixels: Vec<u8>,
}

/// The pixels a renderer or an exporter draws, already sized and rotated.
#[derive(Clone, Debug)]
pub struct ImageBitmap {
    pub width: u32,
    pub height: u32,
    pub pixels: Vec<u8>,
    pub revision: u64,
}

impl ImageBitmap {
    pub fn empty() -> Self {
        Self {
            width: 0,
            height: 0,
            pixels: Vec::new(),
            revision: 0,
        }
    }
}

/// Strip cap's own `file://` scheme. Everything else is handed to the OS as a
/// plain filesystem path: a draft written by the platform carries
/// `file:///data/storage/el2/...`, while tests pass plain relative paths.
pub fn local_path(uri: &str) -> &str {
    uri.strip_prefix("file://").unwrap_or(uri)
}

/// Intrinsic pixel size, read from the file header only.
///
/// `insert_image_clip` needs this to store a size in the draft without paying
/// for a full decode per edit.
pub fn source_dimensions(uri: &str) -> Result<(u32, u32)> {
    let path = local_path(uri);
    if path.trim().is_empty() {
        return Err(Error::invalid("image uri must not be empty"));
    }
    match image::image_dimensions(path) {
        Ok((w, h)) if w > 0 && h > 0 => Ok((w, h)),
        Ok(_) => Err(Error::Decode(format!("image {path} has a zero dimension"))),
        Err(err) => Err(Error::Decode(format!("cannot read image {path}: {err}"))),
    }
}

/// Decode a still into straight-alpha RGBA8.
pub fn load_image(uri: &str) -> Result<Arc<SourceImage>> {
    let path = local_path(uri);
    if path.trim().is_empty() {
        return Err(Error::invalid("image uri must not be empty"));
    }
    let decoded = image::open(path)
        .map_err(|err| Error::Decode(format!("cannot decode image {path}: {err}")))?
        .to_rgba8();
    let (width, height) = decoded.dimensions();
    if width == 0 || height == 0 {
        return Err(Error::Decode(format!("image {path} has a zero dimension")));
    }
    Ok(Arc::new(SourceImage {
        width,
        height,
        pixels: decoded.into_raw(),
    }))
}

/// Revision of the pixels this render would produce, for renderer-side caching.
///
/// A sticker is often swapped for another file of the same size, so the source
/// identity has to be part of the hash; size alone would let a stale texture
/// survive the swap.
pub fn image_revision(source_key: &str, canvas_w: u32, canvas_h: u32, t: &Transform) -> u64 {
    fn hash_text(hash: &mut u64, text: &str) {
        for byte in text.as_bytes() {
            *hash ^= u64::from(*byte);
            *hash = hash.wrapping_mul(0x0000_0100_0000_01b3);
        }
    }
    let mut hash = 0xcbf2_9ce4_8422_2325u64;
    hash_text(&mut hash, source_key);
    for bits in [
        canvas_w as u64,
        canvas_h as u64,
        f32::to_bits(t.scale) as u64,
        f32::to_bits(t.rotation_deg) as u64,
    ] {
        hash ^= bits;
        hash = hash.wrapping_mul(0x0000_0100_0000_01b3);
    }
    hash
}

/// Target pixel size for a still on a canvas.
///
/// Rotation is applied after scaling, so the size reported here is the unrotated
/// one; [`render_image`] expands it to the rotated bounding box.
pub fn fitted_size(
    src_w: u32,
    src_h: u32,
    canvas_w: u32,
    canvas_h: u32,
    t: &Transform,
) -> (u32, u32) {
    if src_w == 0 || src_h == 0 || canvas_w == 0 || canvas_h == 0 {
        return (0, 0);
    }
    let fit = (canvas_w as f32 / src_w as f32).min(canvas_h as f32 / src_h as f32);
    let scale = fit * t.scale;
    let width = (src_w as f32 * scale).round().max(1.0) as u32;
    let height = (src_h as f32 * scale).round().max(1.0) as u32;
    (width, height)
}

/// Resample, rotate and package a still for drawing.
///
/// `source_key` only feeds [`image_revision`]; the pixels themselves decide the
/// output, so a caller can cache by revision and never by path.
pub fn render_image(
    source: &SourceImage,
    canvas_w: u32,
    canvas_h: u32,
    t: &Transform,
    source_key: &str,
) -> Result<ImageBitmap> {
    if canvas_w == 0 || canvas_h == 0 {
        return Err(Error::invalid("canvas must be non-empty"));
    }
    if !t.scale.is_finite() || t.scale <= 0.0 {
        return Err(Error::invalid("image scale must be finite and > 0"));
    }
    let (dst_w, dst_h) = fitted_size(source.width, source.height, canvas_w, canvas_h, t);
    if dst_w == 0 || dst_h == 0 {
        return Err(Error::invalid("image resamples to an empty bitmap"));
    }
    let scaled = resample(source.width, source.height, &source.pixels, dst_w, dst_h);

    let rotation = if t.rotation_deg.is_finite() {
        t.rotation_deg % 360.0
    } else {
        0.0
    };
    let (bitmap_w, bitmap_h, pixels) = if rotation.abs() < 1e-4 {
        (dst_w, dst_h, scaled)
    } else {
        rotate(&scaled, dst_w, dst_h, rotation)
    };

    Ok(ImageBitmap {
        width: bitmap_w,
        height: bitmap_h,
        pixels,
        revision: image_revision(source_key, canvas_w, canvas_h, t),
    })
}

/// Bilinear resample of a straight-alpha RGBA8 image.
fn resample(src_w: u32, src_h: u32, src: &[u8], dst_w: u32, dst_h: u32) -> Vec<u8> {
    if dst_w == src_w && dst_h == src_h {
        return src.to_vec();
    }
    let mut out = vec![0u8; dst_w as usize * dst_h as usize * 4];
    let x_ratio = src_w as f32 / dst_w as f32;
    let y_ratio = src_h as f32 / dst_h as f32;
    for y in 0..dst_h {
        // Pixel centres, not edges: mapping 0 -> 0 would shift a 2x downscale
        // half a source pixel up and left, which shows as a one-pixel border.
        let sy = ((y as f32 + 0.5) * y_ratio - 0.5).max(0.0);
        let y0 = sy.floor() as u32;
        let y1 = (y0 + 1).min(src_h - 1);
        let fy = sy - y0 as f32;
        for x in 0..dst_w {
            let sx = ((x as f32 + 0.5) * x_ratio - 0.5).max(0.0);
            let x0 = sx.floor() as u32;
            let x1 = (x0 + 1).min(src_w - 1);
            let fx = sx - x0 as f32;
            let idx = ((y * dst_w + x) * 4) as usize;
            for channel in 0..4 {
                let p00 = f32::from(src[((y0 * src_w + x0) * 4) as usize + channel]);
                let p10 = f32::from(src[((y0 * src_w + x1) * 4) as usize + channel]);
                let p01 = f32::from(src[((y1 * src_w + x0) * 4) as usize + channel]);
                let p11 = f32::from(src[((y1 * src_w + x1) * 4) as usize + channel]);
                let top = p00 * (1.0 - fx) + p10 * fx;
                let bottom = p01 * (1.0 - fx) + p11 * fx;
                let value = top * (1.0 - fy) + bottom * fy;
                out[idx + channel] = value.round().clamp(0.0, 255.0) as u8;
            }
        }
    }
    out
}

/// Rotate clockwise on screen and expand to the rotated bounding box.
///
/// Positive degrees turn the content clockwise, matching
/// [`Transform::rotation_deg`]. Pixels outside the source map to transparent,
/// which is what makes a rotated sticker show its corners instead of a hard
/// crop.
fn rotate(src: &[u8], src_w: u32, src_h: u32, degrees: f32) -> (u32, u32, Vec<u8>) {
    let radians = degrees.to_radians();
    let (sin, cos) = (radians.sin(), radians.cos());
    let bound_w = (src_w as f32 * cos.abs() + src_h as f32 * sin.abs()).ceil();
    let bound_h = (src_w as f32 * sin.abs() + src_h as f32 * cos.abs()).ceil();
    let dst_w = (bound_w as u32).max(1);
    let dst_h = (bound_h as u32).max(1);
    let mut out = vec![0u8; dst_w as usize * dst_h as usize * 4];

    let (cx, cy) = (src_w as f32 * 0.5, src_h as f32 * 0.5);
    let (dcx, dcy) = (dst_w as f32 * 0.5, dst_h as f32 * 0.5);
    for y in 0..dst_h {
        for x in 0..dst_w {
            // Inverse rotation: from the output centre back into the source.
            let px = x as f32 + 0.5 - dcx;
            let py = y as f32 + 0.5 - dcy;
            let sx = px * cos + py * sin + cx;
            let sy = -px * sin + py * cos + cy;
            if sx < 0.0 || sy < 0.0 || sx >= src_w as f32 || sy >= src_h as f32 {
                continue;
            }
            let idx = ((y * dst_w + x) * 4) as usize;
            let pixel = sample_bilinear(src, src_w, src_h, sx, sy);
            out[idx..idx + 4].copy_from_slice(&pixel);
        }
    }
    (dst_w, dst_h, out)
}

/// Bilinear sample at a continuous source coordinate (pixel centres at
/// `i + 0.5`), clamped to the edges.
fn sample_bilinear(src: &[u8], src_w: u32, src_h: u32, sx: f32, sy: f32) -> [u8; 4] {
    let x = sx - 0.5;
    let y = sy - 0.5;
    let x0 = x.floor();
    let y0 = y.floor();
    let fx = x - x0;
    let fy = y - y0;
    let px0 = (x0.max(0.0) as u32).min(src_w - 1);
    let px1 = ((x0 + 1.0).max(0.0) as u32).min(src_w - 1);
    let py0 = (y0.max(0.0) as u32).min(src_h - 1);
    let py1 = ((y0 + 1.0).max(0.0) as u32).min(src_h - 1);
    let mut out = [0u8; 4];
    for channel in 0..4 {
        let p00 = f32::from(src[((py0 * src_w + px0) * 4) as usize + channel]);
        let p10 = f32::from(src[((py0 * src_w + px1) * 4) as usize + channel]);
        let p01 = f32::from(src[((py1 * src_w + px0) * 4) as usize + channel]);
        let p11 = f32::from(src[((py1 * src_w + px1) * 4) as usize + channel]);
        let top = p00 * (1.0 - fx) + p10 * fx;
        let bottom = p01 * (1.0 - fx) + p11 * fx;
        let value = top * (1.0 - fy) + bottom * fy;
        out[channel] = value.round().clamp(0.0, 255.0) as u8;
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use image::{ImageFormat, Rgba, RgbaImage};
    use std::path::PathBuf;

    /// Scratch directory for fixtures. Relative on purpose: `temp_dir()` on a
    /// Windows host carries a drive colon, which the burn path rejects, and
    /// keeping every test artefact under `target/` makes cleanup trivial.
    fn scratch(name: &str) -> PathBuf {
        let dir = PathBuf::from(format!("target/image-tests/{name}"));
        std::fs::create_dir_all(&dir).expect("scratch dir");
        dir
    }

    /// A 4x4 PNG: opaque red interior, a transparent border and a green corner,
    /// so both the resampler and the alpha path have something to say.
    fn fixture(name: &str) -> String {
        let mut img = RgbaImage::from_pixel(4, 4, Rgba([200, 30, 30, 255]));
        for x in 0..4 {
            img.put_pixel(x, 0, Rgba([0, 0, 0, 0]));
            img.put_pixel(x, 3, Rgba([0, 0, 0, 0]));
        }
        img.put_pixel(0, 0, Rgba([0, 220, 0, 255]));
        let path = scratch(name).join("sticker.png");
        img.save_with_format(&path, ImageFormat::Png)
            .expect("write fixture");
        path.to_str().expect("utf-8 path").to_string()
    }

    fn transform(scale: f32, rotation_deg: f32) -> Transform {
        Transform {
            scale,
            rotation_deg,
            ..Transform::default()
        }
    }

    #[test]
    fn dimensions_come_from_the_header_without_decoding() {
        let uri = fixture("dimensions");
        let (w, h) = source_dimensions(&format!("file://{uri}")).expect("header size");
        assert_eq!((w, h), (4, 4));
        assert!(source_dimensions("target/image-tests/missing.png").is_err());
    }

    #[test]
    fn a_four_by_four_sticker_scales_to_the_canvas_fit_rule() {
        let uri = fixture("fit");
        let source = load_image(&uri).expect("decode");
        // 540x960 canvas: fit = 540/4 = 135, so scale 0.5 is 270 px.
        let t = transform(0.5, 0.0);
        let bitmap = render_image(&source, 540, 960, &t, &uri).expect("render");
        assert_eq!((bitmap.width, bitmap.height), (270, 270));
        assert_eq!(bitmap.pixels.len(), 270 * 270 * 4);
        // The opaque interior survives the upscale; the transparent top row
        // stays transparent instead of turning black.
        let centre = ((135 * 270 + 135) * 4) as usize;
        assert!(
            bitmap.pixels[centre] > 150,
            "red channel {}",
            bitmap.pixels[centre]
        );
        assert_eq!(bitmap.pixels[centre + 3], 255);
        let top = ((4 * 270 + 135) * 4) as usize;
        assert_eq!(bitmap.pixels[top + 3], 0, "alpha must stay straight");
    }

    #[test]
    fn rotation_expands_to_the_bounding_box_and_keeps_corners_transparent() {
        let uri = fixture("rotate");
        let source = load_image(&uri).expect("decode");
        // A 270x270 bitmap turned 30 degrees needs 270*(cos+sin) = 369 px.
        let t = transform(0.5, 30.0);
        let bitmap = render_image(&source, 540, 960, &t, &uri).expect("render");
        assert_eq!((bitmap.width, bitmap.height), (369, 369));
        let corner = 0;
        assert_eq!(
            bitmap.pixels[corner + 3],
            0,
            "rotated corners must be clear"
        );
        let centre = ((184 * 369 + 184) * 4) as usize;
        assert_eq!(bitmap.pixels[centre + 3], 255, "the body must stay opaque");
    }

    #[test]
    fn revision_changes_with_the_source_not_just_the_size() {
        let a = image_revision("file:///a.png", 540, 960, &transform(1.0, 0.0));
        let b = image_revision("file:///b.png", 540, 960, &transform(1.0, 0.0));
        assert_ne!(a, b, "two same-sized stickers must not share a texture");
        assert_eq!(
            a,
            image_revision("file:///a.png", 540, 960, &transform(1.0, 0.0)),
            "same inputs must be stable for the compositor cache"
        );
        assert_ne!(
            a,
            image_revision("file:///a.png", 1080, 960, &transform(1.0, 0.0))
        );
        assert_ne!(
            a,
            image_revision("file:///a.png", 540, 960, &transform(2.0, 0.0))
        );
    }

    #[test]
    fn invalid_inputs_are_rejected_before_any_pixels_move() {
        let uri = fixture("invalid");
        let source = load_image(&uri).expect("decode");
        assert!(render_image(&source, 0, 960, &transform(1.0, 0.0), &uri).is_err());
        assert!(render_image(&source, 540, 960, &transform(0.0, 0.0), &uri).is_err());
        assert!(load_image("target/image-tests/not-an-image.png").is_err());
    }

    #[test]
    fn local_path_strips_only_the_scheme() {
        assert_eq!(local_path("file:///data/x.png"), "/data/x.png");
        assert_eq!(local_path("target/x.png"), "target/x.png");
    }
}
