//! Subtitle text rasterisation.
//!
//! The preview and the exporter must draw the *same* glyphs at the same size,
//! so text is rasterised here, in the platform-independent core, and nowhere
//! else. The preview uploads the bitmap as a GL texture; the exporter writes the
//! same bitmap as a PNG and lets FFmpeg overlay it. Two different consumers,
//! one rasteriser, so a subtitle cannot look different after export.
//!
//! Why not FFmpeg `drawtext`/`libass`: both drag FreeType (and, for `libass`,
//! HarfBuzz/fribidi) into a cross-compiled FFmpeg build for Android and OHOS,
//! while the preview would still need its own text renderer for GL. Rasterising
//! once in Rust keeps the two paths identical and adds no native dependency.
//! Rich typography (kerning, shaping, ruby) is a v2 concern; v1 needs legible
//! CJK subtitles with a hard outline, which is what this module produces.
//!
//! # Fonts
//!
//! No font is bundled: CJK fonts are 10-20 MB and the platforms already ship
//! good ones (`HarmonyOS_Sans_SC` on HarmonyOS, `NotoSansCJK` on Android). The
//! caller may name a font explicitly ([`FontSpec`]); otherwise
//! [`resolve_default_font`] probes the well-known system paths in order.

use crate::{Error, Result};
use fontdue::{Font, FontSettings};
use std::collections::HashMap;
use std::sync::{Arc, Mutex, OnceLock};

/// A font file plus the face inside it.
///
/// `collection_index` matters: `NotoSansCJK-Regular.ttc` is a collection whose
/// face 0 is Japanese. Drawing Chinese with face 0 yields a font that renders
/// the wrong regional glyph shapes (and, on some builds, `notdef`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FontSpec {
    pub path: String,
    pub collection_index: u32,
}

impl FontSpec {
    pub fn new(path: impl Into<String>) -> Self {
        Self {
            path: path.into(),
            collection_index: 0,
        }
    }

    pub fn with_collection_index(mut self, index: u32) -> Self {
        self.collection_index = index;
        self
    }
}

/// Candidate fonts, most specific first.
///
/// HarmonyOS ships `HarmonyOS_Sans_SC.ttf` (Simplified Chinese, TTF, no
/// collection); Android ships `NotoSansCJK-Regular.ttc` (collection, face 2 is
/// the Simplified Chinese one in AOSP's CJK collection layout). Host builds on
/// Windows find Microsoft YaHei so `cargo test` and the demo tools can render
/// text without a device.
pub fn default_font_specs() -> Vec<FontSpec> {
    let mut out = vec![
        FontSpec::new("/system/fonts/HarmonyOS_Sans_SC.ttf"),
        FontSpec::new("/system/fonts/NotoSansCJK-Regular.ttc").with_collection_index(2),
        FontSpec::new("/system/fonts/NotoSansSC-Regular.otf"),
        FontSpec::new("/system/fonts/DroidSansFallback.ttf"),
        FontSpec::new("/data/fonts/HarmonyOS_Sans_SC.ttf"),
    ];
    if cfg!(windows) {
        out.push(FontSpec::new("C:/Windows/Fonts/msyh.ttc").with_collection_index(0));
        out.push(FontSpec::new("C:/Windows/Fonts/simhei.ttf"));
    }
    out
}

/// First candidate that exists on disk. Reported as `NotFound` when none do,
/// which on a device means "this build was shipped without a system font" and
/// the renderer should skip text rather than draw boxes.
pub fn resolve_default_font() -> Result<FontSpec> {
    let mut tried = Vec::new();
    for spec in default_font_specs() {
        if std::fs::metadata(&spec.path)
            .map(|m| m.is_file())
            .unwrap_or(false)
        {
            return Ok(spec);
        }
        tried.push(spec.path);
    }
    Err(Error::not_found(format!(
        "no usable system font (tried {})",
        tried.join(", ")
    )))
}

type FontKey = (String, u32);

static FONTS: OnceLock<Mutex<HashMap<FontKey, Arc<Font>>>> = OnceLock::new();

fn font_cache() -> &'static Mutex<HashMap<FontKey, Arc<Font>>> {
    FONTS.get_or_init(|| Mutex::new(HashMap::new()))
}

/// Load (or re-use) a font. Parsing a 10 MB CJK font takes tens of
/// milliseconds, so the result is cached per `(path, collection_index)`: a
/// 60 Hz preview must not re-parse it per frame.
pub fn load_font(spec: &FontSpec) -> Result<Arc<Font>> {
    let key: FontKey = (spec.path.clone(), spec.collection_index);
    if let Some(hit) = font_cache()
        .lock()
        .map_err(|_| Error::Internal("font cache poisoned".into()))?
        .get(&key)
        .cloned()
    {
        return Ok(hit);
    }

    let bytes = std::fs::read(&spec.path)
        .map_err(|e| Error::Io(format!("cannot read font {}: {e}", spec.path)))?;
    let settings = FontSettings {
        collection_index: spec.collection_index,
        // The rasteriser is told up front which pixel size it is aiming at; the
        // default (40 px) is only a hint, but subtitles live in the 40-80 px
        // range so it is a good one.
        scale: 48.0,
        load_substitutions: true,
    };
    let font = Font::from_bytes(bytes, settings)
        .map_err(|e| Error::Decode(format!("cannot parse font {}: {e}", spec.path)))?;
    let font = Arc::new(font);
    font_cache()
        .lock()
        .map_err(|_| Error::Internal("font cache poisoned".into()))?
        .insert(key, font.clone());
    Ok(font)
}

/// Horizontal alignment of every line inside the rasterised block.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TextAlign {
    Left,
    Center,
    Right,
}

impl TextAlign {
    /// 0.0 for left, 0.5 for centre, 1.0 for right: the fraction of the free
    /// space that goes before the line.
    fn offset_factor(self) -> f32 {
        match self {
            TextAlign::Left => 0.0,
            TextAlign::Center => 0.5,
            TextAlign::Right => 1.0,
        }
    }
}

/// Everything about how a subtitle block looks.
#[derive(Clone, Debug)]
pub struct TextStyle {
    /// Font size in pixels, at timeline resolution (not screen pixels).
    pub font_px: f32,
    /// Ink colour, straight (non-premultiplied) alpha.
    pub color: [u8; 4],
    /// Outline thickness in pixels; 0 disables the outline.
    pub outline_px: f32,
    /// Outline colour, drawn behind the ink.
    pub outline_color: [u8; 4],
    /// Line advance multiplier on top of the font's own line height.
    pub line_spacing: f32,
    /// Wrap width in pixels; the block never exceeds it (plus padding).
    pub max_width_px: f32,
    pub align: TextAlign,
}

impl TextStyle {
    /// The v1 default subtitle look: white text, black 2 px outline, centred,
    /// wrapped at 86% of the canvas width, sized off the canvas *width* so a
    /// portrait and a landscape project read the same.
    ///
    /// Sizes are relative to the timeline canvas because the rasterised bitmap
    /// is in timeline pixels; the compositor then scales it with the layer's
    /// transform.
    pub fn subtitle(canvas_w: u32, canvas_h: u32) -> Self {
        let width = canvas_w.max(1) as f32;
        let height = canvas_h.max(1) as f32;
        let font_px = (width * 0.048).clamp(12.0, height * 0.16);
        Self {
            font_px,
            color: [255, 255, 255, 255],
            outline_px: (font_px * 0.055).clamp(1.0, 6.0),
            outline_color: [0, 0, 0, 235],
            line_spacing: 1.12,
            max_width_px: width * 0.86,
            align: TextAlign::Center,
        }
    }
}

/// RGBA8 pixels, straight alpha, row-major, top row first.
///
/// Straight alpha (not premultiplied) because GL blending is configured as
/// `SRC_ALPHA, ONE_MINUS_SRC_ALPHA` and FFmpeg's `overlay` also expects
/// straight alpha in its RGBA input.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TextBitmap {
    pub width: u32,
    pub height: u32,
    pub pixels: Vec<u8>,
}

impl TextBitmap {
    /// Stable revision for renderer-side caching: same text + canvas + style
    /// version means the same pixels, so a compositor can skip the upload.
    pub fn revision(&self) -> u64 {
        fnv1a(&[
            self.width as u64,
            self.height as u64,
            self.pixels.len() as u64,
        ])
    }

    /// Empty bitmap, used when a renderer wants "draw nothing" without a
    /// special case.
    pub fn empty() -> Self {
        Self {
            width: 0,
            height: 0,
            pixels: Vec::new(),
        }
    }
}

fn fnv1a(values: &[u64]) -> u64 {
    let mut hash = 0xcbf2_9ce4_8422_2325u64;
    for value in values {
        let bytes = value.to_le_bytes();
        for byte in bytes {
            hash ^= byte as u64;
            hash = hash.wrapping_mul(0x0000_0100_0000_01b3);
        }
    }
    hash
}

/// Revision of the *input* of a rasterisation, for caches that want to skip the
/// work entirely. Includes the text bytes so an edit invalidates immediately.
pub fn text_revision(text: &str, style: &TextStyle) -> u64 {
    let mut hash = 0xcbf2_9ce4_8422_2325u64;
    for byte in text.as_bytes() {
        hash ^= *byte as u64;
        hash = hash.wrapping_mul(0x0000_0100_0000_01b3);
    }
    let scalars = [
        style.font_px.to_bits() as u64,
        style.outline_px.to_bits() as u64,
        style.max_width_px.to_bits() as u64,
        style.line_spacing.to_bits() as u64,
        u64::from(style.color[3]),
        u64::from(style.outline_color[3]),
        style.align as u64,
    ];
    fnv1a(&[hash, fnv1a(&scalars)])
}

/// Rasterise one subtitle block.
///
/// `\n` is a hard line break; otherwise lines are broken to fit
/// `style.max_width_px`, preferring a space (Latin) and falling back to a
/// per-character break (CJK, which has no spaces to break at).
pub fn render_text(text: &str, spec: &FontSpec, style: &TextStyle) -> Result<TextBitmap> {
    if text.trim().is_empty() {
        return Err(Error::invalid("subtitle text is empty"));
    }
    if !style.font_px.is_finite() || style.font_px <= 0.0 {
        return Err(Error::invalid("font_px must be positive"));
    }
    if !style.max_width_px.is_finite() || style.max_width_px <= 0.0 {
        return Err(Error::invalid("max_width_px must be positive"));
    }

    let font = load_font(spec)?;
    let px = style.font_px;

    // Line metrics come from the font, so a CJK face with tall ascenders does
    // not clip: every baseline is placed by the font's own numbers.
    let (ascent, descent, line_gap) = match font.horizontal_line_metrics(px) {
        Some(m) => (m.ascent, m.descent, m.line_gap),
        None => (px * 0.8, -px * 0.2, 0.0),
    };
    let line_advance = ((ascent - descent + line_gap) * style.line_spacing.max(1.0)).max(px);
    let line_advance = line_advance.ceil();

    let wrapped = wrap_lines(&font, text, px, style.max_width_px)?;
    let line_count = wrapped.len().max(1);

    let content_w = wrapped
        .iter()
        .map(|line| line.width)
        .fold(0.0f32, f32::max)
        .min(style.max_width_px)
        .max(1.0);
    let pad = style.outline_px.max(0.0).ceil() + 2.0;
    let radius = style.outline_px.max(0.0).round() as i32;

    let width = (content_w.ceil() as i64 + 2 * pad as i64).clamp(1, 8192) as u32;
    let height = (line_advance as i64 * line_count as i64 + 2 * pad as i64).clamp(1, 8192) as u32;

    // Ink coverage first, in a single-channel mask; the outline is a dilation of
    // it, so the two never fight over pixels.
    let mut mask = vec![0u8; width as usize * height as usize];
    for (index, line) in wrapped.iter().enumerate() {
        let baseline = pad + index as f32 * line_advance + ascent;
        let free = content_w - line.width;
        let mut pen = pad + free * style.align.offset_factor();
        for glyph in &line.glyphs {
            let (metrics, bitmap) = font.rasterize(glyph.ch, px);
            // Bitmap row 0 is the glyph's top; `ymin` is the offset from the
            // baseline to the bitmap's bottom edge, positive up.
            let top = baseline - (metrics.ymin as f32 + metrics.height as f32);
            let left = pen + metrics.xmin as f32;
            blit(
                &mut mask,
                width,
                height,
                GlyphPlacement {
                    bitmap: &bitmap,
                    w: metrics.width,
                    h: metrics.height,
                    left,
                    top,
                },
            );
            pen += glyph.advance;
        }
    }

    // Outline: dilate the mask by `radius` with a separable max filter. A box
    // would be two passes too and much cheaper than a disc; at 2-3 px the
    // difference is invisible and the corners stay hard, which is what subtitle
    // outlines look like anyway.
    let outline = if radius > 0 {
        let mut horizontal = vec![0u8; mask.len()];
        for y in 0..height {
            let row = y as usize * width as usize;
            for x in 0..width {
                let start = (x as i32 - radius).max(0) as usize;
                let end = ((x as i32 + radius) as usize).min(width as usize - 1);
                let mut best = 0u8;
                for sx in start..=end {
                    best = best.max(mask[row + sx]);
                }
                horizontal[row + x as usize] = best;
            }
        }
        let mut vertical = vec![0u8; mask.len()];
        for x in 0..width as usize {
            for y in 0..height as usize {
                let start = (y as i32 - radius).max(0) as usize;
                let end = ((y as i32 + radius) as usize).min(height as usize - 1);
                let mut best = 0u8;
                for sy in start..=end {
                    best = best.max(horizontal[sy * width as usize + x]);
                }
                vertical[y * width as usize + x] = best;
            }
        }
        vertical
    } else {
        Vec::new()
    };

    let mut pixels = vec![0u8; width as usize * height as usize * 4];
    for (i, &ink_alpha) in mask.iter().enumerate() {
        let outline_alpha = outline.get(i).copied().unwrap_or(0);
        // Outline under ink, straight-alpha "over".
        let mut out = [0u8, 0, 0, 0];
        if outline_alpha > 0 {
            out[..3].copy_from_slice(&style.outline_color[..3]);
            out[3] = mul_alpha(style.outline_color[3], outline_alpha);
        }
        if ink_alpha > 0 {
            let a_top = mul_alpha(style.color[3], ink_alpha);
            let inv = 1.0 - a_top as f32 / 255.0;
            for (dst, src) in out[..3].iter_mut().zip(style.color[..3].iter()) {
                *dst = (*src as f32 + *dst as f32 * inv).clamp(0.0, 255.0).round() as u8;
            }
            out[3] = (a_top as f32 + out[3] as f32 * inv)
                .clamp(0.0, 255.0)
                .round() as u8;
        }
        let base = i * 4;
        pixels[base..base + 4].copy_from_slice(&out);
    }

    Ok(TextBitmap {
        width,
        height,
        pixels,
    })
}

fn mul_alpha(color_alpha: u8, coverage: u8) -> u8 {
    ((color_alpha as u32 * coverage as u32 + 127) / 255) as u8
}

/// One rasterised glyph and where its top-left corner lands in the mask.
struct GlyphPlacement<'a> {
    bitmap: &'a [u8],
    w: usize,
    h: usize,
    left: f32,
    top: f32,
}

/// Source-over one rasterised glyph into the coverage mask.
fn blit(mask: &mut [u8], width: u32, height: u32, glyph: GlyphPlacement<'_>) {
    let GlyphPlacement {
        bitmap,
        w: glyph_w,
        h: glyph_h,
        left,
        top,
    } = glyph;
    if glyph_w == 0 || glyph_h == 0 {
        return;
    }
    let x0 = left.round() as i64;
    let y0 = top.round() as i64;
    for gy in 0..glyph_h {
        let y = y0 + gy as i64;
        if y < 0 || y >= height as i64 {
            continue;
        }
        let row = y as usize * width as usize;
        for gx in 0..glyph_w {
            let x = x0 + gx as i64;
            if x < 0 || x >= width as i64 {
                continue;
            }
            let coverage = bitmap[gy * glyph_w + gx];
            if coverage == 0 {
                continue;
            }
            let slot = &mut mask[row + x as usize];
            *slot = (*slot).max(coverage);
        }
    }
}

struct Glyph {
    ch: char,
    advance: f32,
}

struct WrappedLine {
    glyphs: Vec<Glyph>,
    width: f32,
}

/// Break `text` into lines that fit `max_width`.
///
/// Explicit `\n` always breaks. A line that overflows breaks at the last space
/// in it when there is one (Latin), otherwise right before the character that
/// would overflow (CJK). Trailing spaces are dropped so a wrapped line never
/// starts with an indent.
fn wrap_lines(font: &Font, text: &str, px: f32, max_width: f32) -> Result<Vec<WrappedLine>> {
    let mut lines = Vec::new();
    for raw in text.split('\n') {
        let mut current: Vec<Glyph> = Vec::new();
        let mut width = 0.0f32;
        let mut last_space: Option<usize> = None;

        for ch in raw.chars() {
            let metrics = font.metrics(ch, px);
            let advance = metrics.advance_width.max(0.0);
            if ch == ' ' || ch == '\t' {
                if !current.is_empty() {
                    last_space = Some(current.len());
                }
                current.push(Glyph { ch, advance });
                width += advance;
                if width > max_width {
                    // A line that is nothing but spaces: keep it, it is an
                    // intentional blank line.
                    if let Some(space) = last_space {
                        let mut rest = current.split_off(space);
                        let head = current;
                        lines.push(WrappedLine {
                            width: head.iter().map(|g| g.advance).sum(),
                            glyphs: head,
                        });
                        // Drop the break space itself.
                        if rest.first().map(|g| g.ch) == Some(' ') {
                            rest.remove(0);
                        }
                        current = rest;
                        width = current.iter().map(|g| g.advance).sum();
                    }
                    last_space = None;
                }
                continue;
            }
            if width + advance > max_width && !current.is_empty() {
                // Prefer a word break; fall back to a per-character break, which
                // is the only option for CJK text (there are no spaces).
                if let Some(space) = last_space {
                    carry_break(&mut current, space, &mut lines);
                    width = current.iter().map(|g| g.advance).sum();
                }
                last_space = None;
                if width + advance > max_width && !current.is_empty() {
                    break_per_char(&mut current, &mut width, &mut lines, ch);
                }
            }
            current.push(Glyph { ch, advance });
            width += advance;
        }

        while current
            .last()
            .map(|g| g.ch)
            .is_some_and(char::is_whitespace)
        {
            // Keep the measured width honest after trimming.
            let last = current.pop().unwrap();
            width -= last.advance;
        }
        lines.push(WrappedLine {
            glyphs: current,
            width: width.max(0.0),
        });
    }
    if lines.is_empty() {
        lines.push(WrappedLine {
            glyphs: Vec::new(),
            width: 0.0,
        });
    }
    Ok(lines)
}

/// Break the current line at `space`, pushing the head and returning the rest.
fn carry_break(current: &mut Vec<Glyph>, space: usize, lines: &mut Vec<WrappedLine>) {
    let mut rest = current.split_off(space);
    let head = std::mem::take(current);
    lines.push(WrappedLine {
        width: head.iter().map(|g| g.advance).sum(),
        glyphs: head,
    });
    if rest.first().map(|g| g.ch) == Some(' ') {
        rest.remove(0);
    }
    *current = rest;
}

/// Per-character break: the overflowing character starts the next line, except
/// that a closing punctuation mark is pulled back with the glyph before it
/// (CJK 禁则), so a line never begins with `。` or `，`.
fn break_per_char(
    current: &mut Vec<Glyph>,
    width: &mut f32,
    lines: &mut Vec<WrappedLine>,
    ch: char,
) {
    if is_no_start(ch) && current.len() > 1 {
        let carry = current.pop().unwrap();
        let carried = carry.advance;
        lines.push(WrappedLine {
            width: (*width - carried).max(0.0),
            glyphs: std::mem::take(current),
        });
        *current = vec![carry];
        *width = carried;
    } else {
        lines.push(WrappedLine {
            width: *width,
            glyphs: std::mem::take(current),
        });
        *width = 0.0;
    }
}

/// Characters that may not begin a line: they attach to the previous glyph.
fn is_no_start(ch: char) -> bool {
    matches!(
        ch,
        '。' | '，'
            | '、'
            | '；'
            | '：'
            | '？'
            | '！'
            | '）'
            | '】'
            | '》'
            | '」'
            | '』'
            | '”'
            | '’'
            | '…'
            | '—'
            | '·'
            | ','
            | '.'
            | ';'
            | ':'
            | '?'
            | '!'
            | ')'
            | ']'
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn test_font() -> Option<FontSpec> {
        if let Ok(path) = std::env::var("CAPTRIM_TEST_FONT") {
            return Some(FontSpec::new(path));
        }
        resolve_default_font().ok()
    }

    #[test]
    fn default_specs_are_absolute_paths() {
        for spec in default_font_specs() {
            assert!(
                spec.path.starts_with('/') || spec.path.starts_with("C:"),
                "{}",
                spec.path
            );
        }
    }

    #[test]
    fn empty_text_is_rejected() {
        let spec = FontSpec::new("/nonexistent/font.ttf");
        let style = TextStyle::subtitle(1080, 1920);
        assert!(render_text("", &spec, &style).is_err());
        assert!(render_text("   \n ", &spec, &style).is_err());
    }

    #[test]
    fn missing_font_is_an_io_error_not_a_panic() {
        let spec = FontSpec::new("/nonexistent/font.ttf");
        let style = TextStyle::subtitle(1080, 1920);
        assert!(matches!(
            render_text("hi", &spec, &style),
            Err(Error::Io(_))
        ));
    }

    #[test]
    fn renders_ink_with_an_outline() {
        let Some(spec) = test_font() else {
            eprintln!("no system font available; skipping rasterisation test");
            return;
        };
        let style = TextStyle::subtitle(1080, 1920);
        let bitmap = render_text("CapTrim 字幕", &spec, &style).unwrap();

        assert!(bitmap.width > 0 && bitmap.height > 0);
        assert_eq!(
            bitmap.pixels.len(),
            bitmap.width as usize * bitmap.height as usize * 4
        );

        let opaque = bitmap.pixels.chunks(4).filter(|p| p[3] > 0).count();
        // Some pixels are ink (white, opaque) and some are outline (black).
        assert!(opaque > 0, "nothing drawn");
        let white = bitmap
            .pixels
            .chunks(4)
            .filter(|p| p[3] > 200 && p[0] > 200 && p[1] > 200 && p[2] > 200)
            .count();
        assert!(white > 0, "no ink pixels");
        let black = bitmap
            .pixels
            .chunks(4)
            .filter(|p| p[3] > 0 && p[0] < 32 && p[1] < 32 && p[2] < 32)
            .count();
        assert!(black > 0, "no outline pixels");
        // Opaque pixels are a minority of the block but not a rounding error.
        let ratio = opaque as f32 / (bitmap.width * bitmap.height) as f32;
        assert!(ratio > 0.02 && ratio < 0.8, "ink ratio {ratio}");
    }

    #[test]
    fn long_text_wraps_to_the_max_width() {
        let Some(spec) = test_font() else {
            return;
        };
        let mut style = TextStyle::subtitle(1080, 1920);
        style.max_width_px = 420.0;
        style.font_px = 48.0;
        let text = "这是一句很长很长很长的字幕文本，用来验证按宽度自动换行是否正确";
        let bitmap = render_text(text, &spec, &style).unwrap();
        assert!(bitmap.width as f32 <= 420.0 + 2.0 * (style.outline_px.ceil() + 2.0) + 1.0);
        // Four short lines would be ~4 * 60 px; a single line ~60 px.
        assert!(
            bitmap.height > 120,
            "height {} suggests no wrap",
            bitmap.height
        );
    }

    #[test]
    fn explicit_newlines_add_lines() {
        let Some(spec) = test_font() else {
            return;
        };
        let style = TextStyle::subtitle(1080, 1920);
        let single = render_text("第一行", &spec, &style).unwrap();
        let double = render_text("第一行\n第二行", &spec, &style).unwrap();
        assert!(
            double.height > single.height,
            "{} vs {}",
            double.height,
            single.height
        );
    }

    #[test]
    fn revision_changes_with_text_and_style() {
        let style = TextStyle::subtitle(1080, 1920);
        let a = text_revision("hello", &style);
        let b = text_revision("hellp", &style);
        assert_ne!(a, b);
        let mut bigger = style.clone();
        bigger.font_px += 1.0;
        assert_ne!(
            text_revision("hello", &style),
            text_revision("hello", &bigger)
        );
    }

    #[test]
    fn alignment_shifts_ink_but_keeps_the_block() {
        let Some(spec) = test_font() else {
            return;
        };
        let mut style = TextStyle::subtitle(1080, 1920);
        style.max_width_px = 600.0;
        style.font_px = 40.0;
        let mut left = style.clone();
        left.align = TextAlign::Left;
        let mut center = style.clone();
        center.align = TextAlign::Center;

        // The block is as wide as its widest line, so a short line has room to
        // move: that free space is what alignment distributes.
        let text = "AB\nWWWWWWWWWWWWWWWW";
        let l = render_text(text, &spec, &left).unwrap();
        let c = render_text(text, &spec, &center).unwrap();
        assert_eq!(l.width, c.width);
        assert_eq!(l.height, c.height);
        let first_ink_x_in_top_half = |b: &TextBitmap| {
            for y in 0..(b.height / 2) as usize {
                for x in 0..b.width as usize {
                    if b.pixels[(y * b.width as usize + x) * 4 + 3] > 0 {
                        return x;
                    }
                }
            }
            usize::MAX
        };
        assert!(
            first_ink_x_in_top_half(&l) < first_ink_x_in_top_half(&c),
            "left {} vs center {}",
            first_ink_x_in_top_half(&l),
            first_ink_x_in_top_half(&c)
        );
    }
}
