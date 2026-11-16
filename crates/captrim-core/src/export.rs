//! Export planning: contiguous frame ranges instead of one JSON per frame.
//!
//! [`crate::render::plan_frame_at_index`] remains the single source of truth for
//! what a frame looks like. An export plan walks the same timeline, finds the
//! frame indices where the active clip set changes, and emits one
//! [`ExportSegment`] per range. Inside a segment the set of active sources is
//! constant and each source instant is a linear function of the output frame,
//! so an exporter can keep decoders open and avoid re-parsing a plan 60 times a
//! second.
//!
//! Segment boundaries are always frame indices, never raw microseconds. A clip
//! that starts between two frame PTS values becomes active on the *first* frame
//! whose PTS is at or after its start; using floor here would silently render
//! one frame of a clip before it begins.

use crate::model::{Id, Timeline, Transform};
use crate::render::{frame_pts_us, plan_frame_at_index, LayerKind};
use crate::time::Rational;
use serde::{Deserialize, Serialize};
use std::collections::BTreeSet;

/// One video contributor that stays active for a whole export segment.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct ExportVideoItem {
    pub clip_id: Id,
    pub source_id: Id,
    pub uri: String,
    pub z: i32,
    pub kind: LayerKind,
    /// Source instant at the segment's first output frame.
    pub source_start_us: i64,
    /// Source instant just after the segment's last output frame, clamped to
    /// the clip's `[in_us, out_us)` range.
    pub source_end_us: i64,
    /// Source microseconds consumed per timeline microsecond. Keep this as a
    /// rational: converting it to f32 before multiplying is how long exports
    /// slowly drift away from the preview.
    pub speed: Rational,
    pub transform: Transform,
    pub effects: Vec<String>,
}

/// One audio contributor that stays active for a whole export segment.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct ExportAudioItem {
    pub clip_id: Id,
    pub source_id: Id,
    pub uri: String,
    pub source_start_us: i64,
    pub source_end_us: i64,
    pub speed: Rational,
    pub volume: f32,
}

/// A frame-aligned range whose active sources do not change.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct ExportSegment {
    pub index: u32,
    /// First output frame index, inclusive.
    pub start_frame: u64,
    /// One past the last output frame index.
    pub end_frame: u64,
    /// Exact PTS of `start_frame`.
    pub start_us: i64,
    /// Exact PTS of `end_frame` (or the first PTS past the timeline end).
    pub end_us: i64,
    pub video: Vec<ExportVideoItem>,
    pub audio: Vec<ExportAudioItem>,
}

impl ExportSegment {
    pub fn frame_count(&self) -> u64 {
        self.end_frame.saturating_sub(self.start_frame)
    }

    pub fn contains_frame(&self, frame_index: u64) -> bool {
        frame_index >= self.start_frame && frame_index < self.end_frame
    }

    /// Exact source instant for an output frame inside this segment.
    ///
    /// Callers should use this instead of recalculating from `source_start_us`
    /// with a float rate: both preview and export then map a frame to the same
    /// source sample.
    pub fn video_source_us_at(
        &self,
        item: &ExportVideoItem,
        frame_rate: &Rational,
        frame_index: u64,
    ) -> i64 {
        source_us_at(
            self,
            item.source_start_us,
            item.source_end_us,
            &item.speed,
            frame_rate,
            frame_index,
        )
    }

    pub fn audio_source_us_at(
        &self,
        item: &ExportAudioItem,
        frame_rate: &Rational,
        frame_index: u64,
    ) -> i64 {
        source_us_at(
            self,
            item.source_start_us,
            item.source_end_us,
            &item.speed,
            frame_rate,
            frame_index,
        )
    }
}

fn source_us_at(
    segment: &ExportSegment,
    start_us: i64,
    end_us: i64,
    speed: &Rational,
    frame_rate: &Rational,
    frame_index: u64,
) -> i64 {
    if !segment.contains_frame(frame_index) {
        return start_us;
    }
    let t_us = frame_pts_us(frame_rate, frame_index);
    let elapsed = t_us.saturating_sub(segment.start_us);
    let mapped = start_us.saturating_add(Rational::scaled_us_to_us(elapsed, speed));
    mapped.clamp(start_us.min(end_us), start_us.max(end_us))
}

/// The full export breakdown for a timeline.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct ExportPlan {
    pub width: u32,
    pub height: u32,
    pub frame_rate: Rational,
    pub duration_us: i64,
    pub frame_count: u64,
    pub segments: Vec<ExportSegment>,
}

impl ExportPlan {
    pub fn to_json(&self) -> crate::Result<String> {
        serde_json::to_string(self).map_err(|e| crate::Error::Internal(e.to_string()))
    }

    pub fn segment_at_frame(&self, frame_index: u64) -> Option<&ExportSegment> {
        self.segments
            .iter()
            .find(|segment| segment.contains_frame(frame_index))
    }
}

impl Timeline {
    /// Convenience wrapper so callers holding a timeline do not need to import
    /// the free function separately.
    pub fn export_plan(&self) -> ExportPlan {
        plan_export(self)
    }
}

/// First frame whose PTS is at or after `t_us` (ceiling division).
pub fn frame_index_ceil(frame_rate: &Rational, t_us: i64) -> u64 {
    if !frame_rate.is_positive() || t_us <= 0 {
        return 0;
    }
    let numerator = t_us as i128 * frame_rate.num as i128;
    let denominator = 1_000_000i128 * frame_rate.den as i128;
    let value = (numerator + denominator - 1) / denominator;
    u64::try_from(value).unwrap_or(u64::MAX)
}

fn source_at_or_clamped(clip: &crate::model::Clip, t_us: i64) -> i64 {
    let last = clip.out_us.saturating_sub(1).max(clip.in_us);
    clip.to_source_us(t_us).clamp(clip.in_us, last)
}

/// Build the export plan. The returned segments cover `[0, frame_count)` with
/// no overlaps and no holes; a range with no active source is a valid gap
/// segment and must be rendered as black/silence, not treated as an error.
pub fn plan_export(timeline: &Timeline) -> ExportPlan {
    let frame_count = timeline.frame_count();
    let duration_us = timeline.duration_us();
    let mut boundaries = BTreeSet::new();
    boundaries.insert(0u64);
    boundaries.insert(frame_count);

    for track in &timeline.tracks {
        for clip in &track.clips {
            boundaries
                .insert(frame_index_ceil(&timeline.frame_rate, clip.start_us).min(frame_count));
            boundaries
                .insert(frame_index_ceil(&timeline.frame_rate, clip.end_us()).min(frame_count));
        }
    }

    let boundaries: Vec<u64> = boundaries.into_iter().collect();
    let mut segments = Vec::new();

    for pair in boundaries.windows(2) {
        let (start_frame, end_frame) = (pair[0], pair[1]);
        if start_frame >= end_frame {
            continue;
        }
        let plan = plan_frame_at_index(timeline, start_frame);
        let mut video = Vec::new();
        let mut audio = Vec::new();

        for layer in &plan.layers {
            let Some((_, clip)) = timeline.find_clip(layer.clip_id) else {
                continue;
            };
            let source_end_us =
                source_at_or_clamped(clip, frame_pts_us(&timeline.frame_rate, end_frame));
            video.push(ExportVideoItem {
                clip_id: layer.clip_id,
                source_id: layer.source_id,
                uri: layer.uri.clone(),
                z: layer.z,
                kind: layer.kind,
                source_start_us: layer.source_us,
                source_end_us,
                speed: clip.speed,
                transform: layer.transform.clone(),
                effects: layer.effects.clone(),
            });
        }

        for mix in &plan.audio {
            let Some((_, clip)) = timeline.find_clip(mix.clip_id) else {
                continue;
            };
            let source_end_us =
                source_at_or_clamped(clip, frame_pts_us(&timeline.frame_rate, end_frame));
            audio.push(ExportAudioItem {
                clip_id: mix.clip_id,
                source_id: mix.source_id,
                uri: mix.uri.clone(),
                source_start_us: mix.source_us,
                source_end_us,
                speed: clip.speed,
                volume: mix.volume,
            });
        }

        segments.push(ExportSegment {
            index: segments.len() as u32,
            start_frame,
            end_frame,
            start_us: plan.t_us,
            end_us: frame_pts_us(&timeline.frame_rate, end_frame),
            video,
            audio,
        });
    }

    ExportPlan {
        width: timeline.width,
        height: timeline.height,
        frame_rate: timeline.frame_rate,
        duration_us,
        frame_count,
        segments,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::{Clip, MediaSource, Track, TrackKind};

    fn timeline_with(clips: Vec<(TrackKind, Clip)>) -> Timeline {
        let mut t = Timeline {
            frame_rate: Rational::new(30, 1).unwrap(),
            ..Timeline::default()
        };
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

    fn clip_ids(segment: &ExportSegment) -> Vec<Id> {
        let mut ids: Vec<Id> = segment.video.iter().map(|item| item.clip_id).collect();
        ids.sort_unstable();
        ids
    }

    #[test]
    fn frame_index_ceil_uses_first_covering_frame() {
        let fps = Rational::new(30, 1).unwrap();
        assert_eq!(frame_index_ceil(&fps, 0), 0);
        assert_eq!(frame_index_ceil(&fps, 33_333), 1);
        // Frame 1 PTS is 33333; a clip starting 1us later is not active there.
        assert_eq!(frame_index_ceil(&fps, 33_334), 2);
        assert_eq!(frame_index_ceil(&fps, 1_000_000), 30);
    }

    #[test]
    fn adjacent_clips_become_two_export_segments() {
        let mut first = Clip::new(1, 0, 1_000_000, 0);
        first.id = 10;
        let mut second = Clip::new(1, 1_000_000, 2_000_000, 1_000_000);
        second.id = 11;
        let t = timeline_with(vec![
            (TrackKind::MainVideo, first),
            (TrackKind::MainVideo, second),
        ]);

        let plan = t.export_plan();
        assert_eq!(plan.frame_count, 60);
        assert_eq!(plan.segments.len(), 2);
        assert_eq!(plan.segments[0].start_frame, 0);
        assert_eq!(plan.segments[0].end_frame, 30);
        assert_eq!(plan.segments[1].start_frame, 30);
        assert_eq!(plan.segments[1].end_frame, 60);
        assert_eq!(clip_ids(&plan.segments[0]), vec![10]);
        assert_eq!(clip_ids(&plan.segments[1]), vec![11]);
        assert_eq!(plan.segments[0].video[0].source_start_us, 0);
        assert_eq!(plan.segments[0].video[0].source_end_us, 999_999);
        assert_eq!(plan.segments[1].video[0].source_start_us, 1_000_000);
        assert_eq!(plan.segments[1].video[0].source_end_us, 1_999_999);
    }

    #[test]
    fn gaps_are_segments_not_errors() {
        let mut clip = Clip::new(1, 0, 1_000_000, 1_000_000);
        clip.id = 12;
        let t = timeline_with(vec![(TrackKind::MainVideo, clip)]);

        let plan = t.export_plan();
        assert_eq!(plan.segments.len(), 2);
        assert!(plan.segments[0].video.is_empty());
        assert!(plan.segments[0].audio.is_empty());
        assert_eq!(clip_ids(&plan.segments[1]), vec![12]);
    }

    #[test]
    fn segment_boundaries_are_frame_aligned_for_ntsc() {
        let fps = Rational::new(30_000, 1001).unwrap();
        let mut t = timeline_with(vec![]);
        t.frame_rate = fps;
        let mut clip = Clip::new(1, 0, 100_000, 33_367);
        clip.id = 13;
        // Put the clip in a track directly so the helper's 30fps default does
        // not leak into this NTSC case.
        t.tracks.push(Track {
            id: 1,
            kind: TrackKind::MainVideo,
            clips: vec![clip],
        });

        let plan = t.export_plan();
        // The clip occupies timeline [33367, 133367) us, which is 4 NTSC
        // frames; only the first two are the leading gap.
        assert_eq!(plan.frame_count, 4);
        assert_eq!(plan.segments.len(), 2);
        assert_eq!(plan.segments[0].end_frame, 2);
        assert_eq!(plan.segments[1].start_frame, 2);
        assert_eq!(clip_ids(&plan.segments[1]), vec![13]);
    }

    #[test]
    fn segment_items_keep_speed_as_a_rational() {
        let mut clip = Clip::new(1, 0, 1_000_000, 0);
        clip.id = 14;
        clip.speed = Rational::new(2, 1).unwrap();
        let t = timeline_with(vec![(TrackKind::MainVideo, clip)]);

        let plan = t.export_plan();
        assert_eq!(plan.segments.len(), 1);
        let segment = &plan.segments[0];
        let item = &segment.video[0];
        assert_eq!(item.speed, Rational::new(2, 1).unwrap());
        assert_eq!(item.source_start_us, 0);
        assert_eq!(item.source_end_us, 999_999);
        assert_eq!(segment.video_source_us_at(item, &t.frame_rate, 7), 466_666);
    }

    #[test]
    fn segment_active_set_matches_frame_plans() {
        let mut video = Clip::new(1, 0, 2_000_000, 0);
        video.id = 20;
        let mut audio = Clip::new(2, 0, 2_000_000, 500_000);
        audio.id = 21;
        let t = timeline_with(vec![
            (TrackKind::MainVideo, video),
            (TrackKind::Audio, audio),
        ]);

        let plan = t.export_plan();
        assert_eq!(plan.segments.len(), 3);
        for segment in &plan.segments {
            let first = plan_frame_at_index(&t, segment.start_frame);
            assert_eq!(clip_ids(segment), {
                let mut ids: Vec<Id> = first.layers.iter().map(|l| l.clip_id).collect();
                ids.sort_unstable();
                ids
            });
            let mut audio_ids: Vec<Id> = segment.audio.iter().map(|a| a.clip_id).collect();
            audio_ids.sort_unstable();
            let mut planned_audio: Vec<Id> = first.audio.iter().map(|a| a.clip_id).collect();
            planned_audio.sort_unstable();
            assert_eq!(audio_ids, planned_audio);
        }
    }

    #[test]
    fn empty_timeline_has_no_segments() {
        let t = timeline_with(vec![]);
        let plan = t.export_plan();
        assert_eq!(plan.frame_count, 0);
        assert!(plan.segments.is_empty());
        assert!(plan.segment_at_frame(0).is_none());
    }

    #[test]
    fn export_plan_serialises_for_platform_bridges() {
        let mut clip = Clip::new(1, 0, 1_000_000, 0);
        clip.id = 30;
        clip.effects.push("flip".into());
        let t = timeline_with(vec![(TrackKind::MainVideo, clip)]);
        let json = t.export_plan().to_json().unwrap();
        assert!(json.contains("\"segments\""), "{json}");
        assert!(json.contains("\"source_end_us\":999999"), "{json}");
        assert!(json.contains("\"effects\":[\"flip\"]"), "{json}");
        let back: ExportPlan = serde_json::from_str(&json).unwrap();
        assert_eq!(back.segments.len(), 1);
    }
}
