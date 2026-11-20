//! Frame planning: the single source of truth for "what does frame N look like".
//!
//! Preview and export both call [`plan_frame`]. Preview composites the result
//! onto a surface as fast as it can and drops frames when it falls behind;
//! export walks every frame index in order and never drops. Because both read
//! the same plan from the same `Timeline`, speed ramps, clip boundaries and
//! layer order cannot diverge between what the user sees and what gets written.
//!
//! Everything here is integer maths. A frame PTS is derived from `frame_index`
//! and `frame_rate`, never accumulated, so a two-hour timeline cannot drift.

use crate::model::{Clip, Id, Timeline, Track, TrackKind, Transform};
use crate::time::Rational;
use serde::{Deserialize, Serialize};

/// Where a layer's pixels come from. The compositor needs this to know whether
/// to run a video decoder, an image loader, or the text renderer.
#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum LayerKind {
    Video,
    Subtitle,
    Overlay,
    /// A still image or sticker, decoded and resampled by the core. It has no
    /// media source and no decoder slot: the compositor uploads the bitmap the
    /// same way it uploads a caption.
    Image,
}

/// One composited layer of one output frame.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct Layer {
    pub clip_id: Id,
    pub source_id: Id,
    /// Source URI, empty when the media table has no entry (draft edited by
    /// hand). The compositor must treat an empty URI as "layer unavailable"
    /// rather than failing the whole frame.
    pub uri: String,
    /// Exact source instant to decode, already mapped through the clip's speed
    /// and clamped to `[in_us, out_us)`.
    pub source_us: i64,
    /// Draw order: 0 is the bottom layer.
    pub z: i32,
    pub kind: LayerKind,
    pub transform: Transform,
    pub opacity: f32,
    pub effects: Vec<String>,
    /// Text to draw, populated only for `LayerKind::Subtitle`. Empty on every
    /// other layer kind so a compositor can switch on kind alone.
    #[serde(default)]
    pub text: String,
}

/// One audio contributor of one output instant.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct AudioMix {
    pub clip_id: Id,
    pub source_id: Id,
    pub uri: String,
    pub source_us: i64,
    /// Per-clip gain, already multiplied by nothing else: the mixer applies
    /// fade envelopes and master volume on top.
    pub volume: f32,
}

/// Everything needed to produce one output frame.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Default)]
pub struct FramePlan {
    /// Timeline instant this frame represents.
    pub t_us: i64,
    pub frame_index: u64,
    /// Output canvas, copied from the timeline so the compositor never needs
    /// a second call to discover a mid-project resolution change.
    pub width: u32,
    pub height: u32,
    /// Bottom-to-top. Empty means "black frame" (a gap), which is a valid and
    /// common state while scrubbing past the end of a clip.
    pub layers: Vec<Layer>,
    /// Every clip audible at this instant, all tracks mixed together.
    pub audio: Vec<AudioMix>,
}

impl FramePlan {
    pub fn is_empty(&self) -> bool {
        self.layers.is_empty() && self.audio.is_empty()
    }

    pub fn to_json(&self) -> crate::Result<String> {
        serde_json::to_string(self).map_err(|e| crate::Error::Internal(e.to_string()))
    }
}

/// Exact PTS of a frame, in microseconds, truncated.
///
/// `k * 1e6 * den / num` in i128: a 3-hour 30000/1001 timeline is frame 324324,
/// well inside i128 and comfortably outside what naive `k as f32 / fps as f32`
/// can represent without drift.
pub fn frame_pts_us(frame_rate: &Rational, frame_index: u64) -> i64 {
    if !frame_rate.is_positive() {
        return 0;
    }
    let numerator = frame_index as i128 * 1_000_000 * frame_rate.den as i128;
    let value = numerator / frame_rate.num as i128;
    i64::try_from(value).unwrap_or(i64::MAX)
}

/// Frame index that is on screen at `t_us`, i.e. floor(t * fps).
pub fn frame_index_at(frame_rate: &Rational, t_us: i64) -> u64 {
    if !frame_rate.is_positive() || t_us <= 0 {
        return 0;
    }
    let numerator = t_us as i128 * frame_rate.num as i128;
    let denominator = 1_000_000i128 * frame_rate.den as i128;
    u64::try_from(numerator / denominator).unwrap_or(0)
}

/// Number of frames needed to cover `duration_us`, i.e. ceil(duration * fps).
pub fn frame_count(frame_rate: &Rational, duration_us: i64) -> u64 {
    if !frame_rate.is_positive() || duration_us <= 0 {
        return 0;
    }
    let numerator = duration_us as i128 * frame_rate.num as i128;
    let denominator = 1_000_000i128 * frame_rate.den as i128;
    // Ceiling division; duration is positive so the sign is not a concern.
    let count = (numerator + denominator - 1) / denominator;
    u64::try_from(count).unwrap_or(0)
}

impl Timeline {
    /// Exact instant of one output frame.
    pub fn frame_pts_us(&self, frame_index: u64) -> i64 {
        frame_pts_us(&self.frame_rate, frame_index)
    }

    pub fn frame_index_at(&self, t_us: i64) -> u64 {
        frame_index_at(&self.frame_rate, t_us)
    }

    pub fn frame_count(&self) -> u64 {
        frame_count(&self.frame_rate, self.duration_us())
    }

    /// Exact plan for one frame index. The export path uses this.
    pub fn plan_frame_at(&self, frame_index: u64) -> FramePlan {
        plan_frame_at_index(self, frame_index)
    }
}

/// Where this layer's pixels come from, which is what decides whether a platform
/// runs a video decoder, an image loader or the text renderer.
///
/// An overlay track is a stacking position, not a media type: it can hold a video
/// inset (picture-in-picture), a still, or a sticker. So the track kind alone
/// cannot answer the question -- a picture-in-picture clip backed by an .mp4 must
/// come out as `Video`, or a compositor that switches on `kind` draws nothing for
/// it. `source_is_video` is that second half of the answer, and it is `true` when
/// the media has not been probed yet: decoding is the only way to find out.
fn layer_kind(track_kind: TrackKind, source_is_video: bool) -> LayerKind {
    match track_kind {
        TrackKind::MainVideo => LayerKind::Video,
        TrackKind::Subtitle => LayerKind::Subtitle,
        TrackKind::Image => LayerKind::Image,
        TrackKind::Overlay => {
            if source_is_video {
                LayerKind::Video
            } else {
                LayerKind::Overlay
            }
        }
        // Audio tracks never contribute pixels; callers filter them out first.
        TrackKind::Audio => LayerKind::Video,
    }
}

/// Does this source have a stream of `wanted`, as far as we currently know?
///
/// A draft can exist before probing, so "unknown" must mean "include it and let
/// the platform decide" instead of silently dropping layers.
fn source_may_have(timeline: &Timeline, source_id: Id, wanted_video: bool) -> bool {
    let Some(source) = timeline.media.iter().find(|m| m.id == source_id) else {
        return true;
    };
    let Some(info) = &source.info else {
        return true;
    };
    if wanted_video {
        info.primary_video().is_some()
    } else {
        info.primary_audio().is_some()
    }
}

fn clamp_source_us(clip: &Clip, source_us: i64) -> i64 {
    // Half-open [in, out): seeking exactly to `out` is past the last frame, and
    // decoders are entitled to return nothing for it.
    let last = clip.out_us.saturating_sub(1).max(clip.in_us);
    source_us.clamp(clip.in_us, last)
}

/// How long the *video* of `clip` keeps drawing past its own out point.
///
/// A dissolve is anchored on the incoming clip, so the outgoing neighbour has
/// to hold its last frame until the window closes. The extension is derived
/// from adjacency alone -- the next clip on this track starts exactly where
/// this one ends *and* carries a transition -- so moving, trimming or
/// removing either clip turns the cross-dissolve back into a plain cut (or a
/// fade from black) with no bookkeeping in those commands. The source time
/// runs past `out_us` here on purpose; `clamp_source_us` freezes it on the
/// last frame, and the audio plan never sees the extension, so sound still
/// cuts exactly at the edit point.
fn video_extension_us(track: &Track, clip: &Clip) -> i64 {
    track
        .clips
        .iter()
        .find(|next| next.start_us == clip.end_us())
        .and_then(|next| next.transition_in.as_ref())
        .map(|transition| transition.duration_us)
        .unwrap_or(0)
}

/// Linear 0..=1 progress through the dissolve this clip opens with.
///
/// The incoming layer ramps up while the outgoing neighbour (extended by
/// [`video_extension_us`]) stays at full opacity underneath, so the composite
/// is exactly `incoming * u + outgoing * (1 - u)` in one draw list. Fading the
/// outgoing layer as well would square the falloff and show the background
/// through the middle of the dissolve.
fn transition_opacity(clip: &Clip, t_us: i64) -> f32 {
    let Some((window_start, window_end)) = clip.transition_window_us() else {
        return 1.0;
    };
    let span = (window_end - window_start).max(1) as f64;
    ((t_us - window_start) as f64 / span).clamp(0.0, 1.0) as f32
}

/// Audio contributors at an arbitrary instant, without frame quantisation.
///
/// The audio renderer pulls at its own callback rate (often 10 ms), which is
/// unrelated to the video frame grid (33 ms at 30 fps). Feeding that instant
/// through `plan_frame` would snap each audio block to a video frame and make
/// source positions jump by a third of a frame. This function deliberately
/// takes the raw `t_us`; frame plans reuse it with a snapped instant so both
/// paths read the same clip boundaries and speed maps.
pub fn plan_audio_at(timeline: &Timeline, t_us: i64) -> Vec<AudioMix> {
    let mut audio = Vec::new();
    for track in &timeline.tracks {
        // Captions and stills own their pixels and have no file to decode, so
        // they can never contribute sound. Skipping them here also keeps them
        // out of the audio plan's `uri`, which would otherwise be an empty
        // string the platform would try to open.
        if matches!(track.kind, TrackKind::Subtitle | TrackKind::Image) {
            continue;
        }
        for clip in &track.clips {
            if !clip.contains(t_us)
                || clip.volume <= 0.0
                || !source_may_have(timeline, clip.source, false)
            {
                continue;
            }
            let source_us = clamp_source_us(clip, clip.to_source_us(t_us));
            let uri = timeline
                .media
                .iter()
                .find(|m| m.id == clip.source)
                .map(|m| m.uri.clone())
                .unwrap_or_default();
            audio.push(AudioMix {
                clip_id: clip.id,
                source_id: clip.source,
                uri,
                source_us,
                volume: clip.volume,
            });
        }
    }
    audio.sort_by_key(|a| a.clip_id);
    audio
}

/// Build the plan for one frame, addressed by index.
///
/// This is the exact API the export path should use. `plan_frame` takes a
/// timestamp and floors it onto the frame grid; feeding a truncated PTS back
/// into that would floor twice and collapse every NTSC frame onto its
/// predecessor (`frame_pts_us(1) == 33_366`, but frame 1 truly starts at
/// 33_366.67, so flooring 33_366 yields frame 0 again).
pub fn plan_frame_at_index(timeline: &Timeline, frame_index: u64) -> FramePlan {
    let snapped_us = frame_pts_us(&timeline.frame_rate, frame_index);

    // Pairs with the clip's timeline start so the draw order below can break
    // ties inside one track without adding a field to the exported layer.
    let mut layers: Vec<(i64, Layer)> = Vec::new();

    for (z, track) in timeline.tracks.iter().enumerate() {
        let z = z as i32;
        for clip in &track.clips {
            // Active window: normally the clip's own [start, end), extended
            // when a dissolve on the next clip holds this one's picture past
            // its out point.
            let extension = video_extension_us(track, clip);
            if snapped_us < clip.start_us || snapped_us >= clip.end_us().saturating_add(extension) {
                continue;
            }
            let source_us = clamp_source_us(clip, clip.to_source_us(snapped_us));
            // A still carries its own file; everything else points at a media
            // source. Both end up in `uri`, which is all a renderer needs to
            // know where the pixels come from.
            let uri = match &clip.image {
                Some(asset) => asset.uri.clone(),
                None => timeline
                    .media
                    .iter()
                    .find(|m| m.id == clip.source)
                    .map(|m| m.uri.clone())
                    .unwrap_or_default(),
            };

            let source_is_video = source_may_have(timeline, clip.source, true);
            // Subtitle and image clips have no media source, so the
            // video-detection gate must not apply to them: an unprobed (or
            // source-less) clip is still a layer the compositor has to draw.
            // Audio tracks contribute no pixels at all.
            let is_subtitle = track.kind == TrackKind::Subtitle;
            let visible = match track.kind {
                TrackKind::Subtitle | TrackKind::Image => clip.transform.scale > 0.0,
                TrackKind::Audio => false,
                _ => source_is_video && clip.transform.scale > 0.0,
            };
            if visible {
                layers.push((
                    clip.start_us,
                    Layer {
                        clip_id: clip.id,
                        source_id: clip.source,
                        uri: uri.clone(),
                        source_us,
                        z,
                        kind: layer_kind(track.kind, source_is_video),
                        transform: clip.transform.clone(),
                        opacity: transition_opacity(clip, snapped_us),
                        effects: clip.effects.clone(),
                        text: if is_subtitle {
                            clip.text.clone().unwrap_or_default()
                        } else {
                            String::new()
                        },
                    },
                ));
            }
        }
    }

    // Track order already gives z, but clips inside one track are unordered in
    // the draft -- and a dissolve deliberately makes two clips of one track
    // active at once, where the incoming one must draw above the outgoing one
    // regardless of which was inserted first (ids do not encode position).
    layers.sort_by(|a, b| {
        a.1.z
            .cmp(&b.1.z)
            .then(a.0.cmp(&b.0))
            .then(a.1.clip_id.cmp(&b.1.clip_id))
    });
    let layers = layers.into_iter().map(|(_, layer)| layer).collect();

    FramePlan {
        t_us: snapped_us,
        frame_index,
        width: timeline.width,
        height: timeline.height,
        layers,
        audio: plan_audio_at(timeline, snapped_us),
    }
}

/// Build the plan for the frame covering `t_us`, flooring to the frame grid.
///
/// Use this while scrubbing or during playback, where the input is a wall-clock
/// derived position rather than an exact frame boundary.
pub fn plan_frame(timeline: &Timeline, t_us: i64) -> FramePlan {
    plan_frame_at_index(timeline, frame_index_at(&timeline.frame_rate, t_us))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::{MediaSource, Track, Transition, TransitionKind};

    fn timeline_with(clips: Vec<(TrackKind, Clip)>) -> Timeline {
        let mut t = Timeline {
            frame_rate: Rational::new(30, 1).unwrap(),
            ..Timeline::default()
        };
        // Media 1 = video with audio, media 2 = audio only.
        t.media.push(MediaSource {
            id: 1,
            uri: "file:///v.mp4".into(),
            info: None,
        });
        t.media.push(MediaSource {
            id: 2,
            uri: "file:///a.m4a".into(),
            info: None,
        });
        for (i, (kind, clip)) in clips.into_iter().enumerate() {
            t.tracks.push(Track {
                id: (i + 1) as Id,
                kind,
                clips: vec![clip],
            });
        }
        t
    }

    #[test]
    fn audio_plan_uses_raw_time_not_the_frame_grid() {
        let mut clip = Clip::new(2, 0, 1_000_000, 0);
        clip.id = 7;
        clip.volume = 0.5;
        let t = timeline_with(vec![(TrackKind::Audio, clip)]);

        // Both instants fall in video frame 1, but an audio renderer calling
        // at 10 ms blocks needs source positions 10 ms and 50 ms apart.
        let a = plan_audio_at(&t, 10_000);
        let b = plan_audio_at(&t, 50_000);
        assert_eq!(a.len(), 1);
        assert_eq!(b.len(), 1);
        assert_eq!(a[0].source_us, 10_000);
        assert_eq!(b[0].source_us, 50_000);
        assert_eq!(a[0].volume, 0.5);
        assert_eq!(a[0].uri, "file:///a.m4a");

        // Frame plans reuse the same function at the snapped instant.
        let frame = plan_frame(&t, 50_000);
        assert_eq!(frame.audio.len(), 1);
        assert_eq!(frame.audio[0].source_us, 33_333);
    }

    #[test]
    fn audio_plan_applies_speed_and_clamps_to_clip_bounds() {
        let mut clip = Clip::new(2, 0, 1_000_000, 0);
        clip.id = 8;
        clip.speed = Rational::new(2, 1).unwrap();
        let t = timeline_with(vec![(TrackKind::Audio, clip)]);
        assert_eq!(plan_audio_at(&t, 50_000)[0].source_us, 100_000);
        // Half a second into a 2x clip is one full second into the source;
        // the clip ends there, so the source is clamped to the last sample.
        let at_end = plan_audio_at(&t, 499_999);
        assert_eq!(at_end.len(), 1);
        assert!(at_end[0].source_us <= 1_000_000);
        assert!(plan_audio_at(&t, 1_000_000).is_empty());
    }

    #[test]
    fn frame_pts_is_exact_for_ntsc() {
        let fps = Rational::new(30_000, 1001).unwrap();
        assert_eq!(frame_pts_us(&fps, 0), 0);
        assert_eq!(frame_pts_us(&fps, 1), 33_366);
        assert_eq!(frame_pts_us(&fps, 2), 66_733);
        // 1000 frames of 1001/30000 s = 33366666.67 us
        assert_eq!(frame_pts_us(&fps, 1000), 33_366_666);
    }

    #[test]
    fn frame_pts_is_exact_for_integer_rates() {
        let fps = Rational::new(25, 1).unwrap();
        assert_eq!(frame_pts_us(&fps, 1), 40_000);
        assert_eq!(frame_pts_us(&fps, 25), 1_000_000);
        assert_eq!(frame_pts_us(&fps, 1500), 60_000_000);
        assert_eq!(frame_count(&fps, 60_000_000), 1500);
    }

    #[test]
    fn frame_count_covers_a_partial_trailing_frame() {
        let fps = Rational::new(30, 1).unwrap();
        // 1.5 frames worth of material still needs 2 frames.
        assert_eq!(frame_count(&fps, 50_000), 2);
        assert_eq!(frame_count(&fps, 0), 0);
        assert_eq!(frame_count(&fps, -5), 0);
    }

    #[test]
    fn frame_index_floors_instead_of_rounding() {
        let fps = Rational::new(30, 1).unwrap();
        assert_eq!(frame_index_at(&fps, 0), 0);
        assert_eq!(frame_index_at(&fps, 33_333), 0); // still frame 0
        assert_eq!(frame_index_at(&fps, 33_334), 1);
        assert_eq!(frame_index_at(&fps, -1), 0);
    }

    #[test]
    fn gap_produces_an_empty_plan_not_an_error() {
        let mut clip = Clip::new(1, 0, 1_000_000, 2_000_000);
        clip.id = 10;
        let t = timeline_with(vec![(TrackKind::MainVideo, clip)]);
        let plan = plan_frame(&t, 0);
        assert!(plan.is_empty());
        assert_eq!(plan.width, 1920);
    }

    #[test]
    fn plan_snaps_to_the_frame_grid() {
        let mut clip = Clip::new(1, 0, 2_000_000, 0);
        clip.id = 10;
        let t = timeline_with(vec![(TrackKind::MainVideo, clip)]);
        // 33_340 us is inside frame 1 (33_333..66_666), so it snaps down.
        let plan = plan_frame(&t, 33_340);
        assert_eq!(plan.frame_index, 1);
        assert_eq!(plan.t_us, 33_333);
    }

    #[test]
    fn speed_ramp_maps_source_time() {
        let mut clip = Clip::new(1, 0, 4_000_000, 0);
        clip.id = 10;
        clip.speed = Rational::new(2, 1).unwrap();
        let t = timeline_with(vec![(TrackKind::MainVideo, clip)]);
        // 1 s into the timeline is 2 s into the source.
        let plan = plan_frame(&t, 1_000_000);
        assert_eq!(plan.layers.len(), 1);
        assert_eq!(plan.layers[0].source_us, 2_000_000);
    }

    #[test]
    fn source_time_never_reaches_out_point() {
        let mut clip = Clip::new(1, 0, 1_000_000, 0);
        clip.id = 10;
        // 3x speed: the last readable timeline instant maps past out_us.
        clip.speed = Rational::new(3, 1).unwrap();
        let t = timeline_with(vec![(TrackKind::MainVideo, clip)]);
        let plan = plan_frame(&t, 333_300);
        assert_eq!(plan.layers.len(), 1);
        assert!(
            plan.layers[0].source_us < 1_000_000,
            "source_us {} must stay below out_us",
            plan.layers[0].source_us
        );
    }

    #[test]
    fn clip_end_is_exclusive() {
        let mut clip = Clip::new(1, 0, 1_000_000, 0);
        clip.id = 10;
        let t = timeline_with(vec![(TrackKind::MainVideo, clip)]);
        assert_eq!(plan_frame(&t, 999_999).layers.len(), 1);
        assert!(plan_frame(&t, 1_000_000).layers.is_empty());
    }

    #[test]
    fn overlay_stacks_above_main_video() {
        let mut main = Clip::new(1, 0, 5_000_000, 0);
        main.id = 10;
        let mut pip = Clip::new(1, 0, 5_000_000, 0);
        pip.id = 20;
        let t = timeline_with(vec![
            (TrackKind::MainVideo, main),
            (TrackKind::Overlay, pip),
        ]);
        let plan = plan_frame(&t, 1_000_000);
        assert_eq!(plan.layers.len(), 2);
        assert_eq!(plan.layers[0].z, 0);
        assert_eq!(plan.layers[0].kind, LayerKind::Video);
        assert_eq!(plan.layers[1].z, 1);
        // Video pixels on an overlay track are still video: the compositor has to
        // run a decoder for them. A platform that read this as "a sticker" showed
        // the main picture only, with no error anywhere -- a picture-in-picture
        // that silently did not exist.
        assert_eq!(plan.layers[1].kind, LayerKind::Video);
    }

    #[test]
    fn an_overlay_that_is_not_video_stays_an_overlay() {
        // The odd corner of the rule: the track is a stacking position, the media
        // decides the kind. Once a probe says there is no video stream, there is
        // nothing to decode and the layer is what the name suggests.
        assert_eq!(layer_kind(TrackKind::Overlay, false), LayerKind::Overlay);
        assert_eq!(layer_kind(TrackKind::Overlay, true), LayerKind::Video);
        assert_eq!(layer_kind(TrackKind::MainVideo, false), LayerKind::Video);
        assert_eq!(layer_kind(TrackKind::Subtitle, true), LayerKind::Subtitle);
    }

    #[test]
    fn audio_track_contributes_sound_but_no_pixels() {
        let mut clip = Clip::new(2, 0, 5_000_000, 0);
        clip.id = 30;
        let t = timeline_with(vec![(TrackKind::Audio, clip)]);
        let plan = plan_frame(&t, 1_000_000);
        assert!(plan.layers.is_empty());
        assert_eq!(plan.audio.len(), 1);
        assert_eq!(plan.audio[0].uri, "file:///a.m4a");
    }

    #[test]
    fn video_clip_also_contributes_its_sound() {
        let mut clip = Clip::new(1, 0, 5_000_000, 0);
        clip.id = 10;
        let t = timeline_with(vec![(TrackKind::MainVideo, clip)]);
        let plan = plan_frame(&t, 1_000_000);
        assert_eq!(plan.layers.len(), 1);
        assert_eq!(plan.audio.len(), 1);
    }

    #[test]
    fn muted_clip_is_not_mixed() {
        let mut clip = Clip::new(1, 0, 5_000_000, 0);
        clip.id = 10;
        clip.volume = 0.0;
        let t = timeline_with(vec![(TrackKind::MainVideo, clip)]);
        let plan = plan_frame(&t, 1_000_000);
        assert_eq!(plan.layers.len(), 1);
        assert!(plan.audio.is_empty());
    }

    #[test]
    fn plan_serialises_for_the_platform_bridges() {
        let mut clip = Clip::new(1, 0, 5_000_000, 0);
        clip.id = 10;
        clip.effects.push("brightness".into());
        let t = timeline_with(vec![(TrackKind::MainVideo, clip)]);
        let json = plan_frame(&t, 1_000_000).to_json().unwrap();
        assert!(json.contains("\"frame_index\":30"), "{json}");
        assert!(json.contains("\"effects\":[\"brightness\"]"), "{json}");
        let back: FramePlan = serde_json::from_str(&json).unwrap();
        assert_eq!(back.layers.len(), 1);
    }

    /// Two adjacent clips sharing one main-video track, the second one
    /// opening a 1 s dissolve: A = [0, 2 s), B = [2 s, 4 s), window
    /// [2 s, 3 s). `a_id` is parameterised because draw order must not
    /// depend on which clip was inserted first.
    fn dissolve_timeline(a_id: Id) -> Timeline {
        let mut t = Timeline {
            frame_rate: Rational::new(30, 1).unwrap(),
            ..Timeline::default()
        };
        t.media.push(MediaSource {
            id: 1,
            uri: "file:///v.mp4".into(),
            info: None,
        });
        let mut a = Clip::new(1, 0, 2_000_000, 0);
        a.id = a_id;
        let mut b = Clip::new(1, 0, 2_000_000, 2_000_000);
        b.id = 11;
        b.transition_in = Some(Transition {
            kind: TransitionKind::Fade,
            duration_us: 1_000_000,
        });
        t.tracks.push(Track {
            id: 1,
            kind: TrackKind::MainVideo,
            clips: vec![a, b],
        });
        t
    }

    #[test]
    fn dissolve_holds_the_outgoing_frame_and_ramps_the_incoming_one() {
        let t = dissolve_timeline(10);

        // Cut instant: the outgoing clip still fills the frame while the
        // incoming one starts fully transparent (so the compositor can skip
        // it) -- no flash of background at the edit point.
        let at_cut = plan_frame(&t, 2_000_000);
        assert_eq!(at_cut.layers.len(), 2);
        assert_eq!(at_cut.layers[0].clip_id, 10);
        assert_eq!(at_cut.layers[0].opacity, 1.0);
        assert_eq!(at_cut.layers[1].clip_id, 11);
        assert_eq!(at_cut.layers[1].opacity, 0.0);

        // Halfway through the window both layers are up: the outgoing one at
        // full strength underneath, the incoming one at exactly half.
        let mid = plan_frame(&t, 2_500_000);
        assert_eq!(mid.layers.len(), 2);
        assert_eq!(mid.layers[0].clip_id, 10);
        assert_eq!(mid.layers[0].opacity, 1.0);
        assert_eq!(mid.layers[1].clip_id, 11);
        assert_eq!(mid.layers[1].opacity, 0.5);

        // Window closed: the held frame drops out on its own, the incoming
        // clip is fully opaque. Total duration never moved.
        let after = plan_frame(&t, 3_000_000);
        assert_eq!(after.layers.len(), 1);
        assert_eq!(after.layers[0].clip_id, 11);
        assert_eq!(after.layers[0].opacity, 1.0);
        assert_eq!(t.duration_us(), 4_000_000);

        // Before the cut nothing anticipates: plain single layer.
        let before = plan_frame(&t, 1_966_666); // frame 59
        assert_eq!(before.layers.len(), 1);
        assert_eq!(before.layers[0].clip_id, 10);
    }

    #[test]
    fn held_outgoing_source_freezes_on_the_last_frame() {
        let t = dissolve_timeline(10);
        // Past the clip's out point the mapped source would run to 2.5 s of
        // material that does not exist; the clamp pins it to the last frame
        // instead, which is exactly what the preview compositor draws.
        let mid = plan_frame(&t, 2_500_000);
        assert_eq!(mid.layers[0].clip_id, 10);
        assert_eq!(mid.layers[0].source_us, 1_999_999);
        let late = plan_frame(&t, 2_966_666); // frame 89, still inside window
        assert_eq!(late.layers[0].source_us, 1_999_999);
    }

    #[test]
    fn audio_cuts_at_the_edit_point_while_the_picture_dissolves() {
        let t = dissolve_timeline(10);
        // Mid-window the outgoing clip's sound is gone even though its
        // picture is still on screen: audio follows `contains`, never the
        // video extension, so a dissolve cannot double-play two clips.
        let mid = plan_frame(&t, 2_500_000);
        assert_eq!(mid.audio.len(), 1);
        assert_eq!(mid.audio[0].clip_id, 11);
        let before = plan_frame(&t, 1_966_666); // frame 59
        assert_eq!(before.audio.len(), 1);
        assert_eq!(before.audio[0].clip_id, 10);
    }

    #[test]
    fn dissolve_without_an_adjacent_predecessor_fades_in_from_black() {
        // No predecessor at all: the same window reads as a fade-in.
        let mut t = Timeline {
            frame_rate: Rational::new(30, 1).unwrap(),
            ..Timeline::default()
        };
        t.media.push(MediaSource {
            id: 1,
            uri: "file:///v.mp4".into(),
            info: None,
        });
        let mut b = Clip::new(1, 0, 2_000_000, 2_000_000);
        b.id = 11;
        b.transition_in = Some(Transition {
            kind: TransitionKind::Fade,
            duration_us: 1_000_000,
        });
        t.tracks.push(Track {
            id: 1,
            kind: TrackKind::MainVideo,
            clips: vec![b],
        });
        assert_eq!(plan_frame(&t, 2_000_000).layers[0].opacity, 0.0);
        assert_eq!(plan_frame(&t, 2_500_000).layers[0].opacity, 0.5);
        assert_eq!(plan_frame(&t, 3_000_000).layers[0].opacity, 1.0);

        // A predecessor that merely ends *near* the cut does not extend: the
        // degrade path is "fade from black", never a stretched neighbour.
        let mut gap_a = Clip::new(1, 0, 1_000_000, 0);
        gap_a.id = 10;
        t.tracks[0].clips.insert(0, gap_a);
        let mid = plan_frame(&t, 2_500_000);
        assert_eq!(
            mid.layers.len(),
            1,
            "non-adjacent neighbour must not extend"
        );
        assert_eq!(mid.layers[0].clip_id, 11);
        assert_eq!(mid.layers[0].opacity, 0.5);
    }

    #[test]
    fn incoming_clip_draws_above_outgoing_whatever_the_insertion_order() {
        // Ids run backwards against time (B was created first). Sorting by id
        // alone would put the incoming clip *under* the held frame and the
        // dissolve would visibly not happen.
        let t = dissolve_timeline(20); // A carries the larger id
        let mid = plan_frame(&t, 2_500_000);
        assert_eq!(mid.layers.len(), 2);
        assert_eq!(mid.layers[0].clip_id, 20, "outgoing stays on the bottom");
        assert_eq!(mid.layers[1].clip_id, 11, "incoming ramps on top");
        assert_eq!(mid.layers[1].opacity, 0.5);
    }

    #[test]
    fn subtitle_layer_is_planned_without_a_media_source() {
        let mut t = Timeline {
            frame_rate: Rational::new(30, 1).unwrap(),
            ..Timeline::default()
        };
        t.tracks.push(Track {
            id: 1,
            kind: TrackKind::MainVideo,
            clips: vec![Clip::new(1, 0, 2_000_000, 0)],
        });
        let mut subtitle = Clip::new(0, 0, 2_000_000, 1_000_000);
        subtitle.id = 99;
        subtitle.text = Some("字幕在此".into());
        t.tracks.push(Track {
            id: 2,
            kind: TrackKind::Subtitle,
            clips: vec![subtitle],
        });
        // No media entry for source 0 -- a video-sourced clip with this source
        // would still be included (unprobed defaults to "may have video"), but
        // the subtitle path must be independent of probing altogether.
        t.media.push(MediaSource {
            id: 1,
            uri: "file:///v.mp4".into(),
            info: None,
        });

        let before = plan_frame_at_index(&t, 0); // frame 0: 0 us, subtitle not started
        assert_eq!(before.layers.len(), 1);
        assert_eq!(before.layers[0].kind, LayerKind::Video);
        assert!(before.layers[0].text.is_empty());

        let during = plan_frame_at_index(&t, 45); // 1.5 s, inside subtitle
        assert_eq!(during.layers.len(), 2);
        let sub = during
            .layers
            .iter()
            .find(|l| l.kind == LayerKind::Subtitle)
            .unwrap();
        assert_eq!(sub.clip_id, 99);
        assert_eq!(sub.text, "字幕在此");
        assert_eq!(sub.z, 1, "subtitle track sits above the main video");
        assert_eq!(sub.uri, "", "text layers carry no media URI");

        let after = plan_frame_at_index(&t, 90); // 3 s, past both clips
        assert!(after.layers.is_empty());
    }
}
