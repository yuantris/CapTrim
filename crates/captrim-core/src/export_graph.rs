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
//! * one picture layer per segment (no `overlay` stacking of clips yet),
//! * identity or single-layer `Transform` (fit, scale, rotate, offset all
//!   translated; multi-layer z-order comes next),
//! * constant opacity (a dissolve segment is refused, not silently flattened),
//! * audio is not compiled yet (the audio mixer is the next step).
//!
//! Every unsupported shape returns an error naming the segment instead of
//! producing a graph that would quietly export the wrong picture.
//!
//! Video: each segment becomes `<input>trim,setpts,<effects>,scale,pad[sN]`,
//! and the segments are joined with `concat`. `setpts` carries the clip's
//! speed as an exact fraction, so a 2x clip consumes twice the source material
//! over the same output frames instead of drifting.

use crate::effects::effects_to_filter_desc;
use crate::export::ExportPlan;
use crate::model::{Timeline, Transform};
use crate::time::Rational;
use crate::Error;

/// How the graph should be encoded. Geometry (canvas, frame rate) comes from
/// the timeline, so the two cannot disagree.
#[derive(Clone, Debug)]
pub struct GraphSettings {
    /// `libx264`, `libx265`, or an empty string for the encoder default.
    pub codec: String,
    /// Filter chain applied once to the joined picture, e.g. `fps=30` or
    /// `fps=30,format=yuv420p`. Empty means `null` (label pass-through).
    pub filter_suffix: String,
}

impl Default for GraphSettings {
    fn default() -> Self {
        Self {
            codec: "libx264".to_string(),
            filter_suffix: String::new(),
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
    pub codec: String,
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
        }
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

    for segment in &plan.segments {
        // A dissolve carries an opacity ramp; flattening it would drop the
        // transition, so refuse it before anything else is considered.
        let any_ramp = segment.video.iter().any(|layer| {
            (layer.opacity_start - 1.0).abs() > f32::EPSILON
                || (layer.opacity_end - 1.0).abs() > f32::EPSILON
        });
        if any_ramp {
            return Err(Error::InvalidArgument(format!(
                "segment {} holds part of a dissolve ({} picture layers, opacity ramp); \
                 dissolves are not compiled yet",
                segment.index,
                segment.video.len()
            )));
        }
        if segment.video.len() != 1 {
            return Err(Error::InvalidArgument(format!(
                "segment {} has {} picture layers; overlay stacking is not compiled yet",
                segment.index,
                segment.video.len()
            )));
        }
        let layer = &segment.video[0];
        let transform = &layer.transform;
        if !transform.scale.is_finite() || transform.scale <= 0.0 {
            return Err(Error::InvalidArgument(format!(
                "segment {} has scale {}; it must be a positive number",
                segment.index, transform.scale
            )));
        }
        if !transform.rotation_deg.is_finite() {
            return Err(Error::InvalidArgument(format!(
                "segment {} has rotation {}",
                segment.index, transform.rotation_deg
            )));
        }

        let input = inputs
            .iter()
            .position(|uri| uri == &layer.uri)
            .unwrap_or_else(|| {
                inputs.push(layer.uri.clone());
                inputs.len() - 1
            });

        let mut chain = format!(
            "[{input}:v]trim=start={}:end={},{}",
            seconds(layer.source_start_us),
            seconds(layer.source_end_us),
            speed_setpts(&layer.speed),
        );
        let effects = effects_to_filter_desc(&layer.effects)?;
        if !effects.is_empty() {
            chain.push(',');
            chain.push_str(&effects);
        }
        if *transform == Transform::default() {
            // Same geometry as the preview: fit inside the canvas, letterbox
            // the rest. The identity transform is the common case, and this
            // chain needs no overlay, so keep it exact and cheap.
            chain.push_str(&format!(
                ",scale={w}:{h}:force_original_aspect_ratio=decrease,\
                 pad={w}:{h}:(ow-iw)/2:(oh-ih)/2,setsar=1[seg{i}]",
                w = timeline.width,
                h = timeline.height,
                i = segment.index
            ));
            chains.push(chain);
            continue;
        }

        // Geometry contract (model::Transform docs, and the GL compositor):
        // fit -> scale -> rotate -> translate, translation in canvas units.
        //
        // `scale=W*scale:H*scale:decrease` reproduces fit*scale without knowing
        // the source size, because the filter fits the input into that box.
        // Rotation then grows the box to the rotated extent (the preview draws
        // the rotated quad, corners and all), and `overlay` places that box's
        // centre at the canvas centre plus (dx, dy), cropping whatever leaves
        // the canvas exactly as the viewport does.
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
        let i = segment.index;
        let frames = (segment.end_frame - segment.start_frame).max(1);
        chain.push_str(&format!("[lay{i}]"));
        chains.push(chain);
        // The backdrop is cut to the segment's exact frame count: an endless
        // `color` plus `shortest=1` ends one frame early, which shaves a frame
        // off the joined picture and desyncs audio later on.
        chains.push(format!(
            "color=black:s={w}x{h}:r={rate},trim=end_frame={frames}[bg{i}]",
            w = timeline.width,
            h = timeline.height,
            rate = rate_string(&timeline.frame_rate),
        ));
        chains.push(format!(
            "[bg{i}][lay{i}]overlay=x='(main_w-overlay_w)/2+({dx})*main_w':\
             y='(main_h-overlay_h)/2+({dy})*main_h':format=auto[ov{i}]",
            dx = transform.dx,
            dy = transform.dy,
        ));
        chains.push(format!("[ov{i}]format=yuv420p[seg{i}]"));
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
        }
        filter.push_str(&format!("concat=n={}:v=1:a=0[joined]", plan.segments.len()));
        "joined".to_string()
    };

    filter.push(';');
    if settings.filter_suffix.is_empty() {
        filter.push_str(&format!("[{joined}]null[vout]"));
    } else {
        filter.push_str(&format!("[{joined}]{}[vout]", settings.filter_suffix));
    }

    Ok(ExportGraph {
        inputs,
        filter_complex: filter,
        video_label: "vout".to_string(),
        codec: settings.codec.clone(),
    })
}

fn seconds(us: i64) -> String {
    format!("{:.6}", us as f64 / 1_000_000.0)
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
    use crate::model::{Clip, MediaSource, Track, TrackKind, Transition, TransitionKind};

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
            graph.filter_complex.contains("concat=n=2:v=1:a=0"),
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
        let chain = graph.filter_complex.split(';').next().unwrap();
        assert!(chain.contains("hflip,negate"), "{chain}");
        assert!(
            chain.find("hflip").unwrap() < chain.find("scale=").unwrap(),
            "effects must run before the fit: {chain}"
        );
    }

    #[test]
    fn a_dissolve_segment_is_refused_instead_of_flattened() {
        let mut second = clip(1, 4, 2_000_000, 4_000_000, 2_000_000);
        second.transition_in = Some(Transition {
            kind: TransitionKind::Fade,
            duration_us: 500_000,
        });
        let timeline = timeline(vec![clip(1, 3, 0, 2_000_000, 0), second]);
        let plan = plan_export(&timeline);
        let err = compile(&timeline, &plan, &GraphSettings::default()).unwrap_err();
        assert!(err.to_string().contains("dissolve"), "{err}");
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

    // The planner already drops a layer whose scale is not positive, so the
    // guard in `compile` is defence in depth for hand-built plans and has no
    // timeline-level test.
}
