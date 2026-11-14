//! Preview state machine: the loop both platforms run, written once.
//!
//! The platform owns the surface, the decoder and the GPU compositor. It does
//! not own the timing policy, because two copies of a drift-correction policy
//! is exactly how Android and HarmonyOS previews end up disagreeing.
//!
//! Typical platform loop:
//!
//! ```text
//! loop {
//!     match session.step(now_wall_us) {
//!         Present(frame) => { decode(frame.layers); composite(); present(); }
//!         WaitFrame(due) => sleep_until(due),
//!         Idle           => sleep_until_any_event(),
//!         End(at)        => stop(),
//!     }
//! }
//! ```

use crate::clock::PlaybackClock;
use crate::model::Timeline;
use crate::render::{plan_frame, FramePlan};
use serde::{Deserialize, Serialize};

/// What the platform loop should do next.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum Step {
    /// Composite and present this frame.
    Present { frame: FramePlan },
    /// The current frame is already on screen and the next one is not due yet.
    WaitFrame { due_wall_us: i64 },
    /// Paused, nothing on screen needs changing.
    Idle,
    /// Playback reached the end of the timeline.
    End { at_us: i64 },
}

/// Owns clock, timeline snapshot and the "which frame is on screen" state.
#[derive(Clone, Debug)]
pub struct PreviewSession {
    timeline: Timeline,
    clock: PlaybackClock,
    /// Last frame handed to the compositor, used to avoid re-presenting and to
    /// count dropped frames.
    last_presented: Option<u64>,
    dropped: u64,
}

impl PreviewSession {
    pub fn new(timeline: Timeline) -> Self {
        Self {
            timeline,
            clock: PlaybackClock::new(0),
            last_presented: None,
            dropped: 0,
        }
    }

    pub fn timeline(&self) -> &Timeline {
        &self.timeline
    }

    /// Replace the timeline after an edit.
    ///
    /// The on-screen frame is invalidated on purpose: a trim or a filter change
    /// must be visible immediately, and a compositor that only redraws on a new
    /// frame index would otherwise keep showing the pre-edit picture until the
    /// playhead moved.
    pub fn set_timeline(&mut self, timeline: Timeline) {
        self.timeline = timeline;
        self.last_presented = None;
    }

    pub fn is_playing(&self) -> bool {
        self.clock.is_playing()
    }

    pub fn position_at(&self, now_wall_us: i64) -> i64 {
        self.clock.position_at(now_wall_us)
    }

    pub fn duration_us(&self) -> i64 {
        self.timeline.duration_us()
    }

    pub fn frame_count(&self) -> u64 {
        self.timeline.frame_count()
    }

    /// Frames the clock skipped over because it fell behind. A steadily growing
    /// counter on a real device means the decoder or the compositor is too slow.
    pub fn dropped_frames(&self) -> u64 {
        self.dropped
    }

    pub fn presented_frame(&self) -> Option<u64> {
        self.last_presented
    }

    pub fn set_playing(&mut self, playing: bool, now_wall_us: i64) {
        if playing {
            // Pressing play while parked on the last frame restarts from the
            // top, which is what every editor does. `seek` clamps to
            // `duration - 1`, so comparing against the duration would never
            // fire; the frame index is the reliable test.
            let position = self.clock.position_at(now_wall_us);
            let last_frame = self.frame_count().saturating_sub(1);
            let parked_at_end = position >= self.duration_us()
                || self.timeline.frame_index_at(position) >= last_frame;
            if parked_at_end && last_frame > 0 {
                self.seek(0, now_wall_us);
            }
            self.clock.play(now_wall_us);
        } else {
            self.clock.pause(now_wall_us);
        }
    }

    /// Move the playhead. Clamped so a paused scrub to the end still shows the
    /// last frame instead of a black one.
    pub fn seek(&mut self, t_us: i64, now_wall_us: i64) {
        let last = self.duration_us().saturating_sub(1).max(0);
        self.clock.seek(t_us.clamp(0, last), now_wall_us);
        self.last_presented = None;
    }

    /// Re-anchor the clock to the audio renderer's position.
    pub fn sync_to_audio(&mut self, audio_position_us: i64, now_wall_us: i64) -> i64 {
        self.clock.sync_to_audio(audio_position_us, now_wall_us)
    }

    pub fn set_slew_limit_us(&mut self, limit_us: i64) {
        self.clock.set_slew_limit_us(limit_us);
    }

    /// Advance the state machine. Call this from the platform's render loop
    /// with a monotonic wall clock in microseconds.
    pub fn step(&mut self, now_wall_us: i64) -> Step {
        let duration = self.duration_us();
        if duration <= 0 {
            return Step::End { at_us: 0 };
        }

        let playing = self.clock.is_playing();
        let position = self.clock.position_at(now_wall_us);

        if playing && position >= duration {
            self.clock.seek(duration, now_wall_us);
            self.clock.pause(now_wall_us);
            return Step::End { at_us: duration };
        }

        let frame_index = self.timeline.frame_index_at(position);

        if self.last_presented == Some(frame_index) {
            if !playing {
                return Step::Idle;
            }
            // Sleep until the next frame boundary, in wall time. The clock rate
            // is ~1 during a slew, so treating the gap as 1:1 is within a frame.
            let next_pts = self.timeline.frame_pts_us(frame_index.saturating_add(1));
            let wait_us = next_pts.saturating_sub(position).max(1);
            return Step::WaitFrame {
                due_wall_us: now_wall_us.saturating_add(wait_us),
            };
        }

        if let Some(last) = self.last_presented {
            if frame_index > last.saturating_add(1) {
                self.dropped = self.dropped.saturating_add(frame_index - last - 1);
            }
        }
        self.last_presented = Some(frame_index);

        Step::Present {
            frame: plan_frame(&self.timeline, position),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::{Clip, MediaSource, Track, TrackKind};
    use crate::time::Rational;

    /// Two seconds of video at 30 fps, i.e. 60 frames.
    fn timeline() -> Timeline {
        let mut t = Timeline {
            frame_rate: Rational::new(30, 1).unwrap(),
            ..Timeline::default()
        };
        t.media.push(MediaSource {
            id: 1,
            uri: "file:///v.mp4".into(),
            info: None,
        });
        t.tracks.push(Track {
            id: 1,
            kind: TrackKind::MainVideo,
            clips: vec![Clip {
                id: 10,
                ..Clip::new(1, 0, 2_000_000, 0)
            }],
        });
        t
    }

    fn present_frame(step: Step) -> FramePlan {
        match step {
            Step::Present { frame } => frame,
            other => panic!("expected Present, got {other:?}"),
        }
    }

    #[test]
    fn first_step_presents_the_first_frame() {
        let mut s = PreviewSession::new(timeline());
        let frame = present_frame(s.step(0));
        assert_eq!(frame.frame_index, 0);
        assert_eq!(frame.layers.len(), 1);
        assert_eq!(s.presented_frame(), Some(0));
    }

    #[test]
    fn paused_session_goes_idle_after_showing_a_frame() {
        let mut s = PreviewSession::new(timeline());
        s.step(0);
        assert_eq!(s.step(1_000_000), Step::Idle);
        assert_eq!(s.dropped_frames(), 0);
    }

    #[test]
    fn playback_presents_each_frame_once() {
        let mut s = PreviewSession::new(timeline());
        s.set_playing(true, 0);
        assert_eq!(present_frame(s.step(0)).frame_index, 0);
        let step = s.step(10_000);
        assert!(
            matches!(step, Step::WaitFrame { .. }),
            "same frame should not be re-presented: {step:?}"
        );
        // 40 ms later the clock is on frame 1 (33.3 ms per frame).
        assert_eq!(present_frame(s.step(40_000)).frame_index, 1);
        assert_eq!(s.dropped_frames(), 0);
    }

    #[test]
    fn wait_frame_points_at_the_next_frame_boundary() {
        let mut s = PreviewSession::new(timeline());
        s.set_playing(true, 0);
        s.step(0);
        match s.step(10_000) {
            Step::WaitFrame { due_wall_us } => {
                // Frame 1 is due at 33_333 us, so 23_333 us after the step.
                assert_eq!(due_wall_us, 10_000 + 23_333);
            }
            other => panic!("expected WaitFrame, got {other:?}"),
        }
    }

    #[test]
    fn falling_behind_drops_frames_and_reports_it() {
        let mut s = PreviewSession::new(timeline());
        s.set_playing(true, 0);
        s.step(0);
        // The loop stalled for 200 ms: 6 frames elapsed, 5 were never drawn.
        let frame = present_frame(s.step(200_000));
        assert_eq!(frame.frame_index, 6);
        assert_eq!(s.dropped_frames(), 5);
    }

    #[test]
    fn playback_ends_at_the_timeline_duration() {
        let mut s = PreviewSession::new(timeline());
        s.set_playing(true, 0);
        s.step(0);
        match s.step(2_500_000) {
            Step::End { at_us } => assert_eq!(at_us, 2_000_000),
            other => panic!("expected End, got {other:?}"),
        }
        assert!(!s.is_playing(), "playback stops at the end");
    }

    #[test]
    fn playing_from_the_end_rewinds_first() {
        let mut s = PreviewSession::new(timeline());
        s.seek(1_999_999, 0);
        s.set_playing(true, 0);
        assert_eq!(s.position_at(0), 0, "should restart from the top");
    }

    #[test]
    fn seek_invalidates_the_on_screen_frame() {
        let mut s = PreviewSession::new(timeline());
        s.step(0);
        s.seek(1_000_000, 0);
        assert_eq!(s.presented_frame(), None);
        assert_eq!(present_frame(s.step(0)).frame_index, 30);
    }

    #[test]
    fn paused_seek_to_the_end_shows_the_last_frame_not_black() {
        let mut s = PreviewSession::new(timeline());
        s.seek(99_000_000, 0);
        let frame = present_frame(s.step(0));
        assert_eq!(frame.layers.len(), 1, "last frame must still have content");
        assert!(frame.frame_index < s.frame_count());
    }

    #[test]
    fn editing_the_timeline_redraws_the_current_frame() {
        let mut s = PreviewSession::new(timeline());
        s.step(0);
        // A filter was added: the plan must be produced again even though the
        // playhead did not move.
        let mut edited = timeline();
        edited.tracks[0].clips[0].effects.push("brightness".into());
        s.set_timeline(edited);
        let frame = present_frame(s.step(0));
        assert_eq!(frame.layers[0].effects, vec!["brightness".to_string()]);
    }

    #[test]
    fn empty_timeline_ends_immediately() {
        let mut s = PreviewSession::new(Timeline::default());
        assert_eq!(s.step(0), Step::End { at_us: 0 });
        assert_eq!(s.frame_count(), 0);
    }

    #[test]
    fn audio_sync_jump_is_reflected_in_the_next_frame() {
        let mut s = PreviewSession::new(timeline());
        s.set_playing(true, 0);
        s.step(0);
        // Audio has run ahead by 500 ms: the clock snaps and the next step is
        // the frame audio is actually on.
        s.sync_to_audio(500_000, 0);
        let frame = present_frame(s.step(0));
        assert_eq!(frame.frame_index, 15);
        assert_eq!(s.dropped_frames(), 14);
    }

    #[test]
    fn seek_before_the_first_frame_is_clamped() {
        let mut s = PreviewSession::new(timeline());
        s.seek(-5_000_000, 0);
        assert_eq!(s.position_at(0), 0);
    }
}
