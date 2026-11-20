//! Subtitle burn-in: turn subtitle clips into still images and one FFmpeg
//! filter graph that overlays them in their time windows.
//!
//! The pixels come from [`crate::text::render_text`], the same rasteriser the
//! preview uploads as a texture, so a burned export cannot show a different
//! glyph, wrap point or outline than the compositor did. What this module adds
//! is the *placement* (`transform.dx`/`dy` on the timeline canvas) and the
//! *timing* (`[start_us, end_us)` as an `enable=between(t,a,b)` window).
//!
//! The image format is PAM (NetPBM P7, `RGB_ALPHA`), not PNG, on purpose:
//! PNG decoding needs zlib, and HarmonyOS ships no `libz.so` at runtime
//! (verified on a Mate 60), so PNG would force every platform to vendor and
//! ship zlib. The `pam` decoder lives in `libavcodec/pnmdec.c` and carries its
//! own alpha channel, which is all the outline needs.
//!
//! Chain shape (the transcode entry point has a single frame input, so the
//! images enter the graph through `movie`, not as a second `-i`):
//!
//! ```text
//! [in]<prefix>[bg0];
//! movie=<path>[src0];[src0]loop=loop=-1:size=1:start=0[img0];
//! [bg0][img0]overlay=x=<x>:y=<y>:enable='between(t,<a>,<b>)'[bg1];
//! ...
//! [bgN]<suffix>[out]
//! ```

use crate::model::{Id, Timeline, TrackKind};
use crate::text::{render_text, FontSpec, TextBitmap, TextStyle};
use crate::{Error, Result};
use std::io::Write;
use std::path::{Path, PathBuf};

/// One subtitle ready to be overlaid by FFmpeg.
#[derive(Clone, Debug, PartialEq)]
pub struct BurnLayer {
    pub clip_id: Id,
    /// Absolute path of the PAM written for this clip.
    pub path: String,
    pub width: u32,
    pub height: u32,
    /// Top-left corner on the timeline canvas, in pixels.
    pub x: i32,
    pub y: i32,
    pub start_us: i64,
    pub end_us: i64,
}

/// Rasterise every subtitle clip of `timeline` into `dir` and describe where
/// and when each one belongs.
///
/// Files are named `sub_<clip_id>.pam`, so re-running an export over the same
/// directory simply overwrites the previous render. Layers come back in draw
/// order (bottom track first), which is the order the overlay chain must apply
/// them in.
pub fn plan_burn(timeline: &Timeline, dir: &Path, font: &FontSpec) -> Result<Vec<BurnLayer>> {
    if timeline.width == 0 || timeline.height == 0 {
        return Err(Error::invalid("timeline canvas must be non-empty"));
    }
    // Checked before touching the filesystem so the failure is the same on
    // every platform: an unusable directory name is an argument error, not an
    // io error that depends on what the OS thinks of the colons.
    let dir_text = dir
        .to_str()
        .ok_or_else(|| Error::invalid("subtitle work dir is not valid UTF-8"))?;
    validate_filter_path(dir_text)?;
    std::fs::create_dir_all(dir)?;
    let style = TextStyle::subtitle(timeline.width, timeline.height);
    let mut layers = Vec::new();

    for track in &timeline.tracks {
        if track.kind != TrackKind::Subtitle {
            continue;
        }
        for clip in &track.clips {
            let Some(text) = clip.text.as_deref() else {
                continue;
            };
            if text.trim().is_empty() {
                continue;
            }
            if clip.end_us() <= clip.start_us {
                continue;
            }
            let bitmap = render_text(text, font, &style)?;
            // Padding is already part of the bitmap, so the outline cannot be
            // clipped by the overlay bounds. Position is centre-anchored, same
            // as the compositor: dx/dy are fractions of the canvas.
            let canvas_w = timeline.width as f32;
            let canvas_h = timeline.height as f32;
            let x = (canvas_w - bitmap.width as f32) * 0.5 + clip.transform.dx * canvas_w;
            let y = (canvas_h - bitmap.height as f32) * 0.5 + clip.transform.dy * canvas_h;
            let path = dir.join(format!("sub_{}.pam", clip.id));
            write_pam(&bitmap, &path)?;
            let path = path
                .to_str()
                .ok_or_else(|| Error::invalid("subtitle path is not valid UTF-8"))?
                .to_string();
            layers.push(BurnLayer {
                clip_id: clip.id,
                path,
                width: bitmap.width,
                height: bitmap.height,
                x: x.round() as i32,
                y: y.round() as i32,
                start_us: clip.start_us,
                end_us: clip.end_us(),
            });
        }
    }
    Ok(layers)
}

/// Build the `[in] ... [out]` graph that draws `layers` over the frame
/// produced by `prefix`.
///
/// `prefix` is the host's own chain (trim/scale/pad); it must leave the frame
/// at the timeline canvas size and starting at timeline time 0, because the
/// enable windows use timeline microseconds as file time. `suffix` runs after
/// the last overlay (e.g. `fps=15,scale=320:180`). Both may be empty.
pub fn burn_filter(layers: &[BurnLayer], prefix: &str, suffix: &str) -> Result<String> {
    for layer in layers {
        validate_filter_path(&layer.path)?;
    }
    if layers.is_empty() {
        return Ok(format!(
            "[in]{}[out]",
            chain_or_null(&[prefix.trim(), suffix.trim()], "null")
        ));
    }
    let mut graph = String::new();
    graph.push_str(&format!("[in]{}[bg0]", chain_or_null(&[prefix.trim()], "")));
    for (index, layer) in layers.iter().enumerate() {
        let secs = |us: i64| format!("{:.6}", us as f64 / 1_000_000.0);
        graph.push_str(&format!(
            ";movie={}[src{index}];[src{index}]loop=loop=-1:size=1:start=0[img{index}]",
            layer.path
        ));
        graph.push_str(&format!(
            ";[bg{index}][img{index}]overlay=x={}:y={}:enable='between(t,{},{})'\
             :eof_action=endall[bg{}]",
            layer.x,
            layer.y,
            secs(layer.start_us),
            secs(layer.end_us),
            index + 1
        ));
    }
    graph.push_str(&format!(";[bg{}]{}[out]", layers.len(), {
        let suffix = suffix.trim();
        if suffix.is_empty() {
            "null".to_string()
        } else {
            suffix.to_string()
        }
    }));
    Ok(graph)
}

/// `prefix` when it is a usable chain, `null` otherwise, without producing
/// `[in][bg0]` (an empty chain is not a valid filter link).
fn chain_or_null(parts: &[&str], fallback: &str) -> String {
    let joined = parts
        .iter()
        .filter(|part| !part.is_empty())
        .copied()
        .collect::<Vec<_>>()
        .join(",");
    if joined.is_empty() {
        fallback.to_string()
    } else {
        joined
    }
}

/// A filter graph carries paths verbatim, so a path containing any character
/// that the graph parser treats as syntax is rejected instead of half-escaped.
/// This is why a burn export is a device-side operation: a Windows path always
/// carries a drive colon.
fn validate_filter_path(path: &str) -> Result<()> {
    const FORBIDDEN: [char; 7] = [':', '\'', ',', ';', '[', ']', '%'];
    if let Some(bad) = path.chars().find(|c| FORBIDDEN.contains(c)) {
        return Err(Error::invalid(format!(
            "subtitle image path {path} contains {bad:?}, which a filter graph cannot carry; \
             use a directory without ':', quotes, commas, brackets or '%'"
        )));
    }
    Ok(())
}

/// PAM is a plain-text header followed by raw RGB_ALPHA rows, top row first.
fn write_pam(bitmap: &TextBitmap, path: &PathBuf) -> Result<()> {
    let header = format!(
        "P7\nWIDTH {}\nHEIGHT {}\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n",
        bitmap.width, bitmap.height
    );
    let mut file = std::io::BufWriter::new(std::fs::File::create(path)?);
    file.write_all(header.as_bytes())?;
    file.write_all(&bitmap.pixels)?;
    file.flush()?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::{Clip, Track, Transform};
    use crate::time::Rational;

    fn test_font() -> Option<FontSpec> {
        crate::text::default_font_specs()
            .into_iter()
            .find(|spec| crate::text::load_font(spec).is_ok())
    }

    /// Scratch directory for tests. Relative on purpose: `std::env::temp_dir()`
    /// carries a drive colon on Windows, and someone has to be allowed to
    /// exercise the happy path.
    fn scratch(name: &str) -> PathBuf {
        let dir = PathBuf::from(format!("target/subtitle-tests/{name}"));
        let _ = std::fs::remove_dir_all(&dir);
        dir
    }

    fn subtitle_timeline() -> Timeline {
        let mut timeline = Timeline {
            width: 1080,
            height: 1920,
            frame_rate: Rational::new(30, 1).unwrap(),
            ..Timeline::default()
        };
        let mut clip = Clip::new(0, 0, 2_000_000, 500_000);
        clip.id = 7;
        clip.text = Some("CapTrim 字幕".into());
        clip.transform = Transform {
            dy: crate::timeline::SUBTITLE_DEFAULT_DY,
            ..Transform::default()
        };
        timeline.tracks.push(Track {
            id: 1,
            kind: TrackKind::Subtitle,
            clips: vec![clip],
        });
        timeline
    }

    #[test]
    fn plan_burn_writes_pam_and_anchors_at_the_canvas_centre() {
        let Some(font) = test_font() else {
            eprintln!("no system font available; skipping burn plan test");
            return;
        };
        let timeline = subtitle_timeline();
        let dir = scratch("pam");
        let layers = plan_burn(&timeline, &dir, &font).unwrap();
        assert_eq!(layers.len(), 1);
        let layer = &layers[0];
        assert_eq!(layer.clip_id, 7);
        assert_eq!((layer.start_us, layer.end_us), (500_000, 2_500_000));

        let bytes = std::fs::read(&layer.path).unwrap();
        let header_end = bytes.windows(7).position(|w| w == b"ENDHDR\n").unwrap() + 7;
        let header = String::from_utf8_lossy(&bytes[..header_end]);
        assert!(header.starts_with("P7\nWIDTH "), "{header}");
        assert!(header.contains("TUPLTYPE RGB_ALPHA"), "{header}");
        assert_eq!(
            bytes.len() - header_end,
            layer.width as usize * layer.height as usize * 4,
            "payload must be RGBA8"
        );

        // dy = 0.36 puts the bitmap's centre at 86% of the canvas height.
        let expected_y = (1920.0 - layer.height as f32) * 0.5 + 0.36 * 1920.0;
        assert_eq!(layer.y, expected_y.round() as i32);
        let centre = layer.y as f32 + layer.height as f32 * 0.5;
        assert!(
            (centre / 1920.0 - 0.86).abs() < 0.01,
            "subtitle centre at {:.3} of the canvas",
            centre / 1920.0
        );
        // dx = 0 centres horizontally.
        let expected_x = (1080.0 - layer.width as f32) * 0.5;
        assert_eq!(layer.x, expected_x.round() as i32);
    }

    #[test]
    fn burn_filter_chains_every_layer_in_order() {
        let layers = vec![
            BurnLayer {
                clip_id: 1,
                path: "/data/tmp/sub_1.pam".into(),
                width: 800,
                height: 200,
                x: 140,
                y: 1500,
                start_us: 0,
                end_us: 2_000_000,
            },
            BurnLayer {
                clip_id: 2,
                path: "/data/tmp/sub_2.pam".into(),
                width: 400,
                height: 100,
                x: 340,
                y: 1600,
                start_us: 1_500_000,
                end_us: 3_250_000,
            },
        ];
        let graph = burn_filter(&layers, "trim=duration=2,setpts=PTS-STARTPTS", "fps=15").unwrap();
        assert!(
            graph.starts_with("[in]trim=duration=2,setpts=PTS-STARTPTS[bg0];"),
            "{graph}"
        );
        assert!(graph.contains("movie=/data/tmp/sub_1.pam[src0]"), "{graph}");
        // `loop=-1` never ends on its own; without eof_action=endall the export
        // would run forever as soon as the main input hits EOF.
        assert!(
            graph.contains(":eof_action=endall[bg1]"),
            "the still image stream must not keep the graph alive: {graph}"
        );
        assert!(
            graph.contains("enable='between(t,0.000000,2.000000)':eof_action=endall[bg1]"),
            "{graph}"
        );
        assert!(
            graph.contains("enable='between(t,1.500000,3.250000)':eof_action=endall[bg2]"),
            "{graph}"
        );
        assert!(graph.ends_with("[bg2]fps=15[out]"), "{graph}");
        // Later layers must be painted on top: the second overlay consumes bg1.
        assert!(graph.contains("[bg1][img1]overlay="), "{graph}");
    }

    #[test]
    fn empty_prefix_and_suffix_still_produce_a_valid_graph() {
        let layers: Vec<BurnLayer> = Vec::new();
        assert_eq!(burn_filter(&layers, "", "").unwrap(), "[in]null[out]");
        assert_eq!(
            burn_filter(&layers, "scale=540:960", "fps=30").unwrap(),
            "[in]scale=540:960,fps=30[out]"
        );
    }

    #[test]
    fn paths_that_would_break_the_graph_are_rejected() {
        let layer = |path: &str| BurnLayer {
            clip_id: 1,
            path: path.to_string(),
            width: 10,
            height: 10,
            x: 0,
            y: 0,
            start_us: 0,
            end_us: 1_000_000,
        };
        for bad in [
            "/data/a:b.pam",
            "/data/a,b.pam",
            "/data/a[b].pam",
            "/data/50%.pam",
        ] {
            let layers = vec![layer(bad)];
            assert!(
                burn_filter(&layers, "", "").is_err(),
                "{bad} must not reach a graph"
            );
        }
        let layers = vec![layer("/data/files/sub_7.pam")];
        assert!(burn_filter(&layers, "", "").is_ok());
    }

    #[test]
    fn clips_without_text_or_duration_are_skipped() {
        let Some(font) = test_font() else {
            eprintln!("no system font available; skipping burn skip test");
            return;
        };
        let mut timeline = subtitle_timeline();
        timeline.tracks[0].clips[0].text = Some("   ".into());
        let mut empty = Clip::new(0, 0, 1_000_000, 0);
        empty.id = 8;
        empty.text = None;
        timeline.tracks[0].clips.push(empty);
        let dir = scratch("skip");
        let layers = plan_burn(&timeline, &dir, &font).unwrap();
        assert!(layers.is_empty());
    }
}
