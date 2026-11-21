//! Compiles an [`ExportPlan`] into an FFmpeg `filter_complex` graph.
//!
//! This is the missing half of export: until now the SDK only handed the
//! platform a "one source + one filter string" command, so a timeline of
//! several clips could never become one file. `ExportPlan` already knows the
//! frame-aligned segments and the source instant of every layer; this module
//! turns that into the graph FFmpeg should run.
//!
//! Scope of the first slice, deliberately narrow and honest:
//!
//! * any number of picture layers per segment, stacked bottom-to-top by `z`,
//! * identity or arbitrary `Transform` (fit, scale, rotate, offset),
//! * constant opacity, and the 0→1 / 1→0 alpha ramps a dissolve produces,
//! * audio is not compiled yet (the audio mixer is the next step).
//!
//! Every unsupported shape returns an error naming the segment instead of
//! producing a graph that would quietly export the wrong picture.
//!
//! Video: each segment starts from a black canvas cut to its frame count and
//! overlays its layers in `z` order. Every layer is
//! `trim,setpts,<effects>,scale,format=rgba,<rotate>,<opacity>` and lands at
//! the canvas centre plus its `(dx, dy)`. `setpts` carries the clip's speed as
//! an exact fraction, so a 2x clip consumes twice the source material over the
//! same output frames instead of drifting. Segments are joined with `concat`.

use crate::effects::effects_to_filter_desc;
use crate::export::{ExportPlan, ExportVideoItem};
use crate::model::Timeline;
use crate::render::LayerKind;
use crate::time::Rational;
use crate::Error;

/// How the graph should be encoded. Geometry (canvas, frame rate) comes from
/// the timeline, so the two cannot disagree.
#[derive(Clone, Debug)]
pub struct GraphSettings {
    /// `libx264`, `libx265`, or an empty string for the encoder default.
    pub codec: String,
    /// Constant-quality target for the encoder (x264/x265 CRF). Ignored when
    /// `codec` is empty. Lower is better quality, 18–28 is the usable band.
    pub crf: u8,
    /// Encoder speed/efficiency trade-off (`ultrafast` … `veryslow`).
    pub preset: String,
    /// AAC bitrate in kbps.
    pub audio_bitrate_kbps: u32,
    /// Filter chain applied once to the joined picture, e.g. `fps=30` or
    /// `fps=30,format=yuv420p`. Empty means no re-timing.
    pub filter_suffix: String,
    /// Directory the rasterised stills (captions, stickers) are written into
    /// before the graph runs. A filter graph cannot draw text or decode a
    /// sticker on its own, so the SDK rasterises them exactly as the preview
    /// does and feeds the bitmaps in through `movie`. `None` refuses a still
    /// layer rather than exporting a picture that silently lost it.
    pub still_dir: Option<std::path::PathBuf>,
    /// Caption font chosen by the host (`captrim_engine_set_text_font`).
    /// `None` resolves the platform default, the same lookup the preview uses.
    pub still_font: Option<crate::text::FontSpec>,
}

impl Default for GraphSettings {
    fn default() -> Self {
        Self {
            codec: "libx264".to_string(),
            crf: 20,
            preset: "veryfast".to_string(),
            audio_bitrate_kbps: 192,
            filter_suffix: String::new(),
            still_dir: None,
            still_font: None,
        }
    }
}

/// Ready-to-run FFmpeg inputs and graph.
#[derive(Clone, Debug, PartialEq)]
pub struct ExportGraph {
    /// One `-i` per entry, in order. Segments refer to these by index.
    pub inputs: Vec<String>,
    /// The `-filter_complex` value.
    pub filter_complex: String,
    /// Output label to `-map`, always `vout`.
    pub video_label: String,
    /// Output audio label, `None` when the timeline carries no audio at all.
    pub audio_label: Option<String>,
    /// Exact output length. Passed as `-t` so the container cannot end on an
    /// AAC encoder tail one frame past the last planned picture frame.
    pub duration_us: i64,
    pub codec: String,
    pub crf: u8,
    pub preset: String,
    pub audio_bitrate_kbps: u32,
}

impl ExportGraph {
    /// The full argument list, ready for `Command::new("ffmpeg")`.
    pub fn args(&self, dst: &str) -> Vec<String> {
        let mut args = Vec::new();
        for input in &self.inputs {
            args.push("-i".to_string());
            args.push(input.clone());
        }
        args.push("-filter_complex".to_string());
        args.push(self.filter_complex.clone());
        args.push("-map".to_string());
        args.push(format!("[{}]", self.video_label));
        if !self.codec.is_empty() {
            args.push("-c:v".to_string());
            args.push(self.codec.clone());
            // CRF and preset are x264/x265 options; a hardware encoder rejects
            // them, so they are only passed to the software encoders.
            if self.codec.starts_with("libx26") {
                args.push("-crf".to_string());
                args.push(self.crf.to_string());
                args.push("-preset".to_string());
                args.push(self.preset.clone());
            }
        }
        if let Some(audio_label) = &self.audio_label {
            args.push("-map".to_string());
            args.push(format!("[{audio_label}]"));
            args.push("-c:a".to_string());
            args.push("aac".to_string());
            args.push("-b:a".to_string());
            args.push(format!("{}k", self.audio_bitrate_kbps));
        }
        args.push("-t".to_string());
        args.push(seconds(self.duration_us));
        args.push("-y".to_string());
        args.push(dst.to_string());
        args
    }
}

/// Compile the picture side of `plan` into a graph.
pub fn compile(
    timeline: &Timeline,
    plan: &ExportPlan,
    settings: &GraphSettings,
) -> crate::Result<ExportGraph> {
    if plan.segments.is_empty() {
        return Err(Error::InvalidArgument("export plan has no segments".into()));
    }
    if timeline.width == 0 || timeline.height == 0 {
        return Err(Error::InvalidArgument("canvas must be non-empty".into()));
    }

    let mut inputs: Vec<String> = Vec::new();
    let mut chains: Vec<String> = Vec::new();
    // Captions and stickers are stills with no media stream to trim: the SDK
    // rasterises them with the same code the preview uploads as a texture, so
    // they enter this graph the way the burn path feeds them — `movie` + a
    // looped single frame + `overlay` in their time window. They are rasterised
    // once per compile (a still can span several segments) and reused.
    let has_stills = plan.segments.iter().any(|segment| {
        segment
            .video
            .iter()
            .any(|layer| matches!(layer.kind, LayerKind::Subtitle | LayerKind::Image))
    });
    let stills = match (&settings.still_dir, has_stills) {
        (Some(dir), true) => {
            crate::subtitles::plan_burn_with_font(timeline, dir, settings.still_font.as_ref())?
        }
        _ => Vec::new(),
    };
    // Audio is compiled only when the timeline has any: an all-silent project
    // must not gain an empty AAC track.
    let has_audio = plan
        .segments
        .iter()
        .any(|segment| !segment.audio.is_empty());

    for segment in &plan.segments {
        if segment.video.is_empty() {
            return Err(Error::InvalidArgument(format!(
                "segment {} has no picture layers",
                segment.index
            )));
        }
        let i = segment.index;
        let frames = (segment.end_frame - segment.start_frame).max(1);
        let duration_s = frames as f64 / fps_value(&timeline.frame_rate);

        // Every segment starts from a black canvas cut to its exact frame
        // count, then stacks its layers bottom-to-top. An endless `color` plus
        // `shortest=1` would end one frame early and desync the whole export,
        // so the backdrop is trimmed instead.
        chains.push(format!(
            "color=black:s={w}x{h}:r={rate},trim=end_frame={frames}[bg{i}]",
            w = timeline.width,
            h = timeline.height,
            rate = rate_string(&timeline.frame_rate),
        ));
        let mut base = format!("bg{i}");

        let mut layers: Vec<&ExportVideoItem> = segment.video.iter().collect();
        // `z` is the compositor's stacking order; the preview draws the same
        // list in the same order, so equal values keep the planner's order.
        layers.sort_by_key(|layer| layer.z);

        for (depth, layer) in layers.iter().enumerate() {
            // Subtitles and stickers: no stream to trim, so they are overlaid
            // from their rasterised PAM, in the same z position as any other
            // layer. `enable` takes segment-local time — every segment chain
            // starts at 0 after `setpts`/`trim`, and `concat` then stitches the
            // segments back into timeline time.
            if matches!(layer.kind, LayerKind::Subtitle | LayerKind::Image) {
                let Some(still) = stills.iter().find(|still| still.clip_id == layer.clip_id) else {
                    return Err(Error::InvalidArgument(format!(
                        "segment {i} layer {depth} is a {:?} still but the export has no work \
                         dir to rasterise it into; pass a writable directory (captions and \
                         stickers are drawn from PAM bitmaps, not from a media stream)",
                        layer.kind
                    )));
                };
                // A still may outlive the segment it sits in: clamp its window
                // to this segment and skip the segments it does not touch.
                let window_start = still.start_us.max(segment.start_us) - segment.start_us;
                let window_end = still.end_us.min(segment.end_us) - segment.start_us;
                if window_end <= window_start {
                    continue;
                }
                chains.push(format!(
                    "movie={}[st{i}_{depth}];[st{i}_{depth}]loop=loop=-1:size=1:start=0[img{i}_{depth}]",
                    // The filter graph parser eats a backslash, so a Windows
                    // host must see forward slashes; device paths already use
                    // them. `validate_burn_dir` has already rejected the
                    // characters that cannot be carried at all.
                    still.path.replace('\\', "/")
                ));
                let stacked = format!("a{i}_{depth}");
                chains.push(format!(
                    "[{base}][img{i}_{depth}]overlay=x={x}:y={y}:\
                     enable='between(t,{a},{b})':eof_action=endall[{stacked}]",
                    x = still.x,
                    y = still.y,
                    a = seconds(window_start),
                    b = seconds(window_end),
                ));
                base = stacked;
                continue;
            }
            let transform = &layer.transform;
            if !transform.scale.is_finite() || transform.scale <= 0.0 {
                return Err(Error::InvalidArgument(format!(
                    "segment {i} layer {depth} has scale {}; it must be a positive number",
                    transform.scale
                )));
            }
            if !transform.dx.is_finite() || !transform.dy.is_finite() {
                return Err(Error::InvalidArgument(format!(
                    "segment {i} layer {depth} has a non-finite offset"
                )));
            }
            if !transform.rotation_deg.is_finite() {
                return Err(Error::InvalidArgument(format!(
                    "segment {i} layer {depth} has rotation {}",
                    transform.rotation_deg
                )));
            }

            let input = inputs
                .iter()
                .position(|uri| uri == &layer.uri)
                .unwrap_or_else(|| {
                    inputs.push(layer.uri.clone());
                    inputs.len() - 1
                });

            // A dissolve holds the outgoing picture on its last frame while the
            // incoming one fades in, so its source range collapses to a single
            // instant (`start == end`). Trimming there yields an empty stream
            // and the outgoing picture would vanish under the fade. Take the
            // frame that covers that instant (one output frame back, because
            // `start` is exclusive on frame PTS), clone it for the segment and
            // drop the rest; the clone runs before the opacity ramp so a held
            // layer would still fade if it ever carries one.
            let held = layer.source_end_us <= layer.source_start_us;
            let frame_us = 1_000_000.0 / fps_value(&timeline.frame_rate);
            let mut chain = if held {
                format!(
                    "[{input}:v]trim=start={start},trim=end_frame=1,setpts=PTS-STARTPTS,\
                     tpad=stop_mode=clone:stop_duration={duration:.6},trim=end_frame={frames}",
                    start = seconds((layer.source_start_us - frame_us.round() as i64).max(0)),
                    duration = duration_s + 0.1,
                    frames = frames,
                )
            } else {
                format!(
                    "[{input}:v]trim=start={}:end={},{}",
                    seconds(layer.source_start_us),
                    seconds(layer.source_end_us),
                    speed_setpts(&layer.speed),
                )
            };
            let effects = effects_to_filter_desc(&layer.effects)?;
            if !effects.is_empty() {
                chain.push(',');
                chain.push_str(&effects);
            }

            // Geometry contract (model::Transform docs, and the GL compositor):
            // fit -> scale -> rotate -> translate, translation in canvas units.
            //
            // `scale=W*scale:H*scale:decrease` reproduces fit*scale without
            // knowing the source size, because the filter fits the input into
            // that box. Rotation then grows the box to the rotated extent (the
            // preview draws the rotated quad, corners and all), and `overlay`
            // places that box's centre at the canvas centre plus (dx, dy),
            // cropping whatever leaves the canvas exactly as the viewport does.
            let scale = transform.scale as f64;
            let box_w = ceil_px(timeline.width as f64 * scale);
            let box_h = ceil_px(timeline.height as f64 * scale);
            chain.push_str(&format!(
                ",scale={box_w}:{box_h}:force_original_aspect_ratio=decrease,setsar=1,format=rgba"
            ));
            let radians = transform.rotation_deg as f64 * std::f64::consts::PI / 180.0;
            if transform.rotation_deg != 0.0 {
                chain.push_str(&format!(
                    ",rotate={radians}:ow='abs(iw*cos({r}))+abs(ih*sin({r}))':\
                     oh='abs(iw*sin({r}))+abs(ih*cos({r}))':c=none",
                    r = radians
                ));
            }
            if let Some(opacity) =
                opacity_filter(layer.opacity_start, layer.opacity_end, duration_s)?
            {
                chain.push(',');
                chain.push_str(&opacity);
            }

            let layer_label = format!("l{i}_{depth}");
            chain.push_str(&format!("[{layer_label}]"));
            chains.push(chain);

            let stacked = format!("a{i}_{depth}");
            chains.push(format!(
                "[{base}][{layer_label}]overlay=x='(main_w-overlay_w)/2+({dx})*main_w':\
                 y='(main_h-overlay_h)/2+({dy})*main_h':format=auto[{stacked}]",
                dx = transform.dx,
                dy = transform.dy,
            ));
            base = stacked;
        }

        chains.push(format!("[{base}]format=yuv420p[seg{i}]"));

        if has_audio {
            audio_chains(segment, &mut inputs, &mut chains, i, duration_s)?;
        }
    }

    let mut filter = String::new();
    for chain in &chains {
        if !filter.is_empty() {
            filter.push(';');
        }
        filter.push_str(chain);
    }
    let joined = if plan.segments.len() == 1 {
        format!("seg{}", plan.segments[0].index)
    } else {
        filter.push(';');
        for segment in &plan.segments {
            filter.push_str(&format!("[seg{}]", segment.index));
            if has_audio {
                filter.push_str(&format!("[aud{}]", segment.index));
            }
        }
        filter.push_str(&format!(
            "concat=n={}:v=1:a={}[joined][jaudio]",
            plan.segments.len(),
            u8::from(has_audio)
        ));
        "joined".to_string()
    };

    // Frame count has to match the plan exactly: `fps` re-times the joined
    // stream and can emit one extra frame past the last planned one when the
    // overlay timestamps land a hair early, so the tail is trimmed to the
    // plan's own end frame. Nothing is invented and nothing is dropped.
    let total_frames = plan
        .segments
        .last()
        .map(|segment| segment.end_frame)
        .unwrap_or(0);
    filter.push(';');
    let retimed = if settings.filter_suffix.is_empty() {
        joined
    } else {
        filter.push_str(&format!("[{joined}]{}[retimed];", settings.filter_suffix));
        "retimed".to_string()
    };
    filter.push_str(&format!("[{retimed}]trim=end_frame={total_frames}[vout]"));

    let audio_label = has_audio.then(|| {
        let joined_audio = if plan.segments.len() == 1 {
            format!("aud{}", plan.segments[0].index)
        } else {
            "jaudio".to_string()
        };
        // Every segment already carries 48k stereo, but the seam between two
        // segments can still land mid-sample; one last resample pins the rate
        // before AAC sees it.
        filter.push_str(&format!(";[{joined_audio}]aresample=48000[aout]"));
        "aout".to_string()
    });

    Ok(ExportGraph {
        inputs,
        filter_complex: filter,
        video_label: "vout".to_string(),
        audio_label,
        duration_us: plan
            .segments
            .last()
            .map(|segment| segment.end_us)
            .unwrap_or(0),
        codec: settings.codec.clone(),
        crf: settings.crf,
        preset: settings.preset.clone(),
        audio_bitrate_kbps: settings.audio_bitrate_kbps,
    })
}

fn seconds(us: i64) -> String {
    format!("{:.6}", us as f64 / 1_000_000.0)
}

/// Audio for one segment: every contributor trimmed to its source range, sped
/// to the clip's speed, gained by its own (possibly ramping) volume, mixed
/// without normalisation, then padded/trimmed to the segment's exact duration
/// so `concat` sees picture and sound of the same length.
fn audio_chains(
    segment: &crate::export::ExportSegment,
    inputs: &mut Vec<String>,
    chains: &mut Vec<String>,
    i: u32,
    duration_s: f64,
) -> crate::Result<()> {
    let mut labels = Vec::new();
    for (n, item) in segment.audio.iter().enumerate() {
        let input = inputs
            .iter()
            .position(|uri| uri == &item.uri)
            .unwrap_or_else(|| {
                inputs.push(item.uri.clone());
                inputs.len() - 1
            });
        // Audio keeps reading past an out point inside a dissolve window, so
        // the range is normally non-empty; a collapsed one still needs a
        // sample to stay on the timeline rather than shifting the mix.
        let start = item.source_start_us;
        let end = item.source_end_us.max(start.saturating_add(1));
        let mut chain = format!(
            "[{input}:a]atrim=start={}:end={},asetpts=PTS-STARTPTS",
            seconds(start),
            seconds(end),
        );
        if item.speed != Rational::ONE {
            chain.push(',');
            chain.push_str(&atempo_chain(&item.speed)?);
        }
        if let Some(gain) = gain_filter(item.volume, item.volume_end, duration_s)? {
            chain.push(',');
            chain.push_str(&gain);
        }
        let label = format!("au{i}_{n}");
        chain.push_str(&format!("[{label}]"));
        chains.push(chain);
        labels.push(label);
    }

    let mixed = match labels.len() {
        0 => {
            chains.push(format!(
                "anullsrc=r=48000:cl=stereo,atrim=duration={duration_s:.6}[mix{i}]"
            ));
            format!("mix{i}")
        }
        1 => labels.pop().expect("one label"),
        _ => {
            let pads: String = labels.iter().map(|label| format!("[{label}]")).collect();
            chains.push(format!(
                "{pads}amix=inputs={}:normalize=0:duration=longest[mix{i}]",
                labels.len()
            ));
            format!("mix{i}")
        }
    };
    chains.push(format!(
        "[{mixed}]aresample=48000,aformat=channel_layouts=stereo,apad,\
         atrim=duration={duration_s:.6}[aud{i}]"
    ));
    Ok(())
}

/// `atempo` only accepts 0.5..=2.0 per instance, so a 4x clip becomes two
/// instances. Speed is applied here, not by resampling, which keeps pitch.
fn atempo_chain(speed: &Rational) -> crate::Result<String> {
    let mut factor = speed.num as f64 / speed.den as f64;
    if !factor.is_finite() || factor <= 0.0 {
        return Err(Error::InvalidArgument(format!(
            "clip speed {} is not usable for audio",
            speed.num as f64 / speed.den as f64
        )));
    }
    let mut parts = Vec::new();
    while factor > 2.0 {
        parts.push("atempo=2.0".to_string());
        factor /= 2.0;
    }
    while factor < 0.5 {
        parts.push("atempo=0.5".to_string());
        factor /= 0.5;
    }
    parts.push(format!("atempo={factor:.6}"));
    Ok(parts.join(","))
}

/// Gain for one contributor over one segment. The preview ramps solve's audio
/// gain linearly in time, so a segment whose endpoints differ gets the same
/// ramp here; a constant volume needs no expression at all.
fn gain_filter(start: f32, end: f32, duration_s: f64) -> crate::Result<Option<String>> {
    for value in [start, end] {
        if !value.is_finite() || value < 0.0 {
            return Err(Error::InvalidArgument(format!(
                "audio gain {value} is not a finite, non-negative number"
            )));
        }
    }
    if (start - end).abs() <= 1e-6 {
        return Ok(if (start - 1.0).abs() <= 1e-6 {
            None
        } else {
            Some(format!("volume={start:.4}"))
        });
    }
    Ok(Some(format!(
        "volume=eval=frame:volume='{start:.4}+({end:.4}-{start:.4})*(t/{duration_s:.6})'"
    )))
}

/// Even pixel size for a `scale` target: odd sizes break 4:2:0 chroma.
fn ceil_px(size: f64) -> u32 {
    let rounded = size.ceil() as u32;
    if rounded.is_multiple_of(2) {
        rounded
    } else {
        rounded + 1
    }
}

/// `30` or `30000/1001`, the form `color=`/`fps=` accept.
fn rate_string(rate: &Rational) -> String {
    if rate.den == 1 {
        rate.num.to_string()
    } else {
        format!("{}/{}", rate.num, rate.den)
    }
}

fn fps_value(rate: &Rational) -> f64 {
    rate.num as f64 / rate.den as f64
}

/// Alpha for one layer over one segment.
///
/// The preview multiplies each layer's own alpha by an opacity that is linear
/// in frame time, so export has to reproduce that ramp, not just its start.
/// `colorchannelmixer=aa` covers a constant, and the two ramps that actually
/// occur today -- a dissolve's incoming 0→1 and outgoing 1→0 -- are exactly
/// `fade`'s alpha mode. Any other ramp is refused with both endpoints named
/// rather than silently drawn at the wrong level.
fn opacity_filter(start: f32, end: f32, duration_s: f64) -> crate::Result<Option<String>> {
    for value in [start, end] {
        if !value.is_finite() || !(0.0..=1.0).contains(&value) {
            return Err(Error::InvalidArgument(format!(
                "layer opacity {value} is outside 0..=1"
            )));
        }
    }
    const EPS: f32 = 1e-6;
    if (start - end).abs() <= EPS {
        return Ok(if start >= 1.0 - EPS {
            None
        } else {
            Some(format!("colorchannelmixer=aa={start:.4}"))
        });
    }
    if start <= EPS && (end - 1.0).abs() <= EPS {
        return Ok(Some(format!("fade=t=in:st=0:d={duration_s:.6}:alpha=1")));
    }
    if (start - 1.0).abs() <= EPS && end <= EPS {
        return Ok(Some(format!("fade=t=out:st=0:d={duration_s:.6}:alpha=1")));
    }
    Err(Error::InvalidArgument(format!(
        "opacity ramp {start} -> {end} is not compiled yet; only constant, 0->1 and 1->0 \
         are supported"
    )))
}

/// `setpts` for a clip's speed: timeline time = source time / speed, so the
/// PTS scale factor is `1/speed`, kept as an exact fraction.
fn speed_setpts(speed: &Rational) -> String {
    if *speed == Rational::ONE {
        return "setpts=PTS-STARTPTS".to_string();
    }
    match speed.reciprocal() {
        Some(inverse) => format!("setpts=(PTS-STARTPTS)*{}/{}", inverse.num, inverse.den),
        None => "setpts=PTS-STARTPTS".to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::export::plan_export;
    use crate::model::{
        Clip, MediaSource, Track, TrackKind, Transform, Transition, TransitionKind,
    };

    fn timeline(clips: Vec<Clip>) -> Timeline {
        let mut timeline = Timeline {
            width: 540,
            height: 960,
            frame_rate: Rational::new(30, 1).unwrap(),
            ..Timeline::default()
        };
        timeline.media.push(MediaSource {
            id: 1,
            uri: "file:///v.mp4".into(),
            info: None,
        });
        timeline.tracks.push(Track {
            id: 2,
            kind: TrackKind::MainVideo,
            clips,
        });
        timeline
    }

    /// Clips need distinct ids: the exporter resolves each layer back to its
    /// clip to clamp the source range, so duplicates would point at the wrong
    /// one. An editor assigns these; a hand-built timeline must too.
    fn clip(source: u64, id: u64, in_us: i64, out_us: i64, start_us: i64) -> Clip {
        let mut clip = Clip::new(source, in_us, out_us, start_us);
        clip.id = id;
        clip
    }

    #[test]
    fn two_segments_compile_to_one_concat() {
        let timeline = timeline(vec![
            clip(1, 3, 0, 2_000_000, 0),
            clip(1, 4, 2_000_000, 4_000_000, 2_000_000),
        ]);
        let plan = plan_export(&timeline);
        let graph = compile(&timeline, &plan, &GraphSettings::default()).unwrap();

        // One file reused twice must be opened once and referenced twice.
        assert_eq!(graph.inputs, vec!["file:///v.mp4".to_string()]);
        // The end is exclusive, so the last source instant is end - 1us.
        assert!(
            graph
                .filter_complex
                .contains("trim=start=0.000000:end=1.999999"),
            "{}",
            graph.filter_complex
        );
        assert!(
            graph
                .filter_complex
                .contains("trim=start=2.000000:end=3.999999"),
            "{}",
            graph.filter_complex
        );
        assert!(
            graph.filter_complex.contains("concat=n=2:v=1:a=1"),
            "{}",
            graph.filter_complex
        );
        // Letterboxed into the canvas, square pixels.
        assert!(
            graph.filter_complex.contains("scale=540:960"),
            "{}",
            graph.filter_complex
        );
        assert!(
            graph.filter_complex.contains("[vout]"),
            "{}",
            graph.filter_complex
        );
        assert_eq!(graph.args("/tmp/out.mp4").last().unwrap(), "/tmp/out.mp4");
    }

    #[test]
    fn speed_becomes_an_exact_pts_fraction() {
        let mut clip = clip(1, 3, 0, 4_000_000, 0);
        clip.speed = Rational::new(2, 1).unwrap();
        let timeline = timeline(vec![clip]);
        let plan = plan_export(&timeline);
        let graph = compile(&timeline, &plan, &GraphSettings::default()).unwrap();
        assert!(
            graph.filter_complex.contains("setpts=(PTS-STARTPTS)*1/2"),
            "{}",
            graph.filter_complex
        );
    }

    #[test]
    fn effects_are_inlined_before_the_geometry() {
        let mut clip = clip(1, 3, 0, 2_000_000, 0);
        clip.effects = vec!["hflip".into(), "invert".into()];
        let timeline = timeline(vec![clip]);
        let plan = plan_export(&timeline);
        let graph = compile(&timeline, &plan, &GraphSettings::default()).unwrap();
        let chain = graph
            .filter_complex
            .split(';')
            .find(|chain| chain.contains("hflip"))
            .expect("a chain carrying the effects");
        assert!(chain.contains("hflip,negate"), "{chain}");
        assert!(
            chain.find("hflip").unwrap() < chain.find("scale=").unwrap(),
            "effects must run before the fit: {chain}"
        );
    }

    #[test]
    fn a_dissolve_stacks_both_pictures_and_ramps_the_incoming_one() {
        let mut second = clip(1, 4, 2_000_000, 4_000_000, 2_000_000);
        second.transition_in = Some(Transition {
            kind: TransitionKind::Fade,
            duration_us: 500_000,
        });
        let timeline = timeline(vec![clip(1, 3, 0, 2_000_000, 0), second]);
        let plan = plan_export(&timeline);
        let graph = compile(&timeline, &plan, &GraphSettings::default()).unwrap();
        let filter = &graph.filter_complex;
        // The window segment holds both clips: the outgoing picture stays
        // opaque underneath, the incoming one fades 0 -> 1 on top of it.
        let window = plan
            .segments
            .iter()
            .find(|segment| segment.video.len() == 2)
            .expect("a dissolve segment");
        assert!(filter.contains("fade=t=in:st=0:d="), "{filter}");
        // The outgoing picture has no source left to read (its range collapsed
        // to the out point), so it must be frozen and cloned, not dropped.
        assert!(
            filter.contains("tpad=stop_mode=clone:stop_duration="),
            "the outgoing picture must hold its last frame: {filter}"
        );
        // Its frame count is the fade's duration: 0.5s at 30fps.
        // Inside the window both clips sound: the outgoing gain ramps 1 -> 0
        // while the incoming ramps 0 -> 1, so neither sum nor normalisation
        // may touch the levels.
        assert!(filter.contains("amix=inputs=2:normalize=0"), "{filter}");
        assert!(filter.contains("volume=eval=frame:volume='"), "{filter}");
        assert!(
            window.end_frame - window.start_frame == 15,
            "window should be 15 frames, got {}",
            window.end_frame - window.start_frame
        );
        assert_eq!(
            filter.matches("overlay=").count(),
            plan.segments.iter().map(|s| s.video.len()).sum::<usize>()
        );
    }

    #[test]
    fn layers_stack_bottom_to_top_in_z_order() {
        let mut timeline = timeline(vec![clip(1, 3, 0, 2_000_000, 0)]);
        timeline.media.push(MediaSource {
            id: 2,
            uri: "file:///pip.mp4".into(),
            info: None,
        });
        // A picture-in-picture track sits above the main video track (z order
        // follows track order), so the PiP must be the second overlay.
        let mut pip = clip(2, 5, 0, 2_000_000, 0);
        pip.transform = Transform {
            scale: 0.25,
            dx: 0.3,
            dy: -0.3,
            ..Transform::default()
        };
        timeline.tracks.push(Track {
            id: 6,
            kind: TrackKind::Overlay,
            clips: vec![pip],
        });
        let plan = plan_export(&timeline);
        assert_eq!(plan.segments[0].video.len(), 2, "two layers in one segment");
        let graph = compile(&timeline, &plan, &GraphSettings::default()).unwrap();
        let filter = &graph.filter_complex;
        let first = filter
            .find("[bg0][l0_0]overlay=")
            .expect("bottom layer first");
        let second = filter
            .find("[a0_0][l0_1]overlay=")
            .expect("top layer second");
        assert!(first < second, "{filter}");
        assert!(
            filter.contains("scale=136:240"),
            "PiP box at 0.25: {filter}"
        );
    }

    #[test]
    fn scale_places_a_fitted_layer_inside_a_scaled_canvas() {
        let mut clip = clip(1, 3, 0, 2_000_000, 0);
        clip.transform = Transform {
            scale: 0.5,
            ..Transform::default()
        };
        let timeline = timeline(vec![clip]);
        let plan = plan_export(&timeline);
        let graph = compile(&timeline, &plan, &GraphSettings::default()).unwrap();
        let filter = &graph.filter_complex;
        // fit(540x960) * 0.5 == fit into 270x480, so the filter does the work
        // and the compiler never needs the source size.
        assert!(
            filter.contains("scale=270:480:force_original_aspect_ratio=decrease"),
            "{filter}"
        );
        assert!(filter.contains("color=black:s=540x960:r=30"), "{filter}");
        assert!(
            filter.contains("overlay=x='(main_w-overlay_w)/2+(0)*main_w'"),
            "{filter}"
        );
    }

    #[test]
    fn rotation_is_clockwise_and_keeps_the_whole_quad() {
        let mut clip = clip(1, 3, 0, 2_000_000, 0);
        clip.transform = Transform {
            rotation_deg: 30.0,
            ..Transform::default()
        };
        let timeline = timeline(vec![clip]);
        let plan = plan_export(&timeline);
        let graph = compile(&timeline, &plan, &GraphSettings::default()).unwrap();
        let filter = &graph.filter_complex;
        let radians = 30.0_f64 * std::f64::consts::PI / 180.0;
        assert!(filter.contains(&format!("rotate={radians}:")), "{filter}");
        assert!(
            filter.contains("ow='abs(iw*cos(") && filter.contains("oh='abs(iw*sin("),
            "rotation must grow the box to the rotated extent: {filter}"
        );
        assert!(
            filter.contains(":c=none"),
            "rotated corners stay transparent: {filter}"
        );
    }

    #[test]
    fn offset_is_canvas_normalised_with_down_positive() {
        let mut clip = clip(1, 3, 0, 2_000_000, 0);
        clip.transform = Transform {
            dx: 0.25,
            dy: -0.1,
            ..Transform::default()
        };
        let timeline = timeline(vec![clip]);
        let plan = plan_export(&timeline);
        let graph = compile(&timeline, &plan, &GraphSettings::default()).unwrap();
        let filter = &graph.filter_complex;
        assert!(filter.contains("+(0.25)*main_w"), "{filter}");
        assert!(filter.contains("+(-0.1)*main_h"), "{filter}");
    }

    #[test]
    fn a_still_layer_needs_a_work_dir_and_is_drawn_when_it_has_one() {
        let Some(font) = crate::text::default_font_specs()
            .into_iter()
            .find(|spec| crate::text::load_font(spec).is_ok())
        else {
            eprintln!("no system font available; skipping still-layer test");
            return;
        };
        let mut timeline = timeline(vec![clip(1, 3, 0, 2_000_000, 0)]);
        let mut text = Clip::new(0, 0, 1_000_000, 0);
        text.id = 7;
        text.text = Some("字幕".to_string());
        timeline.tracks.push(Track {
            id: 8,
            kind: TrackKind::Subtitle,
            clips: vec![text],
        });
        let plan = plan_export(&timeline);

        // Without a directory there is nowhere to write the bitmap: refuse and
        // name the segment instead of exporting a picture without the caption.
        let err = compile(&timeline, &plan, &GraphSettings::default()).unwrap_err();
        assert!(err.to_string().contains("work dir"), "{err}");

        let dir = std::path::PathBuf::from("target/export-graph-tests/stills");
        let _ = std::fs::remove_dir_all(&dir);
        let graph = compile(
            &timeline,
            &plan,
            &GraphSettings {
                still_dir: Some(dir),
                still_font: Some(font),
                ..GraphSettings::default()
            },
        )
        .unwrap();
        let filter = &graph.filter_complex;
        // The caption sits on the canvas from 0, so its window is the whole
        // segment and the overlay must be enabled there.
        assert!(filter.contains("movie="), "{filter}");
        assert!(filter.contains("loop=loop=-1:size=1:start=0"), "{filter}");
        assert!(
            filter.contains("enable='between(t,0.000000,") && filter.contains("eof_action=endall"),
            "{filter}"
        );
        assert_eq!(graph.inputs.len(), 1, "the still must not add an input");

        // A caption is drawn above the video layer it stacks over: the overlay
        // consumes the label the previous layer produced.
        let video_overlay = filter.find("overlay=x='(main_w-overlay_w)/2").unwrap();
        let still_overlay = filter.rfind("eof_action=endall").unwrap();
        assert!(video_overlay < still_overlay, "{filter}");
    }

    #[test]
    fn an_extra_audio_track_is_mixed_without_normalisation() {
        let mut timeline = timeline(vec![clip(1, 3, 0, 2_000_000, 0)]);
        let mut music = clip(1, 9, 0, 2_000_000, 0);
        music.volume = 0.4;
        timeline.tracks.push(Track {
            id: 10,
            kind: TrackKind::Audio,
            clips: vec![music],
        });
        let plan = plan_export(&timeline);
        let graph = compile(&timeline, &plan, &GraphSettings::default()).unwrap();
        let filter = &graph.filter_complex;
        assert_eq!(graph.audio_label.as_deref(), Some("aout"));
        assert!(filter.contains("amix=inputs=2:normalize=0"), "{filter}");
        assert!(filter.contains("volume=0.4000"), "{filter}");
        assert!(filter.contains("aresample=48000"), "{filter}");
        let args = graph.args("out.mp4");
        assert!(
            args.windows(2).any(|w| w[0] == "-map" && w[1] == "[aout]"),
            "{args:?}"
        );
        assert!(
            args.windows(2).any(|w| w[0] == "-c:a" && w[1] == "aac"),
            "{args:?}"
        );
    }

    #[test]
    fn a_silent_timeline_gets_no_audio_track() {
        // A picture-only source (no audio stream in the probe) must not gain
        // an empty AAC track just because the graph has room for one.
        let mut timeline = timeline(vec![clip(1, 3, 0, 2_000_000, 0)]);
        let mut info = crate::media::MediaInfo {
            uri: "file:///v.mp4".into(),
            duration_us: 4_000_000,
            ..crate::media::MediaInfo::default()
        };
        info.streams.push(crate::media::StreamInfo {
            kind: crate::media::StreamKind::Video,
            width: Some(640),
            height: Some(360),
            ..crate::media::StreamInfo::default()
        });
        timeline.media[0].info = Some(info);
        let plan = plan_export(&timeline);
        let graph = compile(&timeline, &plan, &GraphSettings::default()).unwrap();
        assert!(graph.audio_label.is_none());
        assert!(!graph.filter_complex.contains("concat=n=1:v=1:a=1"));
    }

    #[test]
    fn encoder_settings_are_forwarded_and_hardware_encoders_skip_crf() {
        let timeline = timeline(vec![clip(1, 3, 0, 2_000_000, 0)]);
        let plan = plan_export(&timeline);
        let graph = compile(
            &timeline,
            &plan,
            &GraphSettings {
                crf: 23,
                preset: "medium".to_string(),
                audio_bitrate_kbps: 128,
                ..GraphSettings::default()
            },
        )
        .unwrap();
        let args = graph.args("out.mp4");
        assert!(
            args.windows(2).any(|w| w[0] == "-crf" && w[1] == "23"),
            "{args:?}"
        );
        assert!(
            args.windows(2)
                .any(|w| w[0] == "-preset" && w[1] == "medium"),
            "{args:?}"
        );
        assert!(
            args.windows(2).any(|w| w[0] == "-b:a" && w[1] == "128k"),
            "{args:?}"
        );
        assert!(
            args.windows(2).any(|w| w[0] == "-t"),
            "exact duration: {args:?}"
        );

        let hardware = compile(
            &timeline,
            &plan,
            &GraphSettings {
                codec: "h264_mediacodec".to_string(),
                ..GraphSettings::default()
            },
        )
        .unwrap();
        let args = hardware.args("out.mp4");
        assert!(
            !args.iter().any(|a| a == "-crf"),
            "hardware encoder: {args:?}"
        );
    }

    // The planner already drops a layer whose scale is not positive, so the
    // guard in `compile` is defence in depth for hand-built plans and has no
    // timeline-level test.
}
