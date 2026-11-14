//! Playback clock.
//!
//! Audio is the master clock: it is the only stream that cannot be resampled
//! on the fly without an audible artefact, so video follows audio and never the
//! other way round. [`PlaybackClock::sync_to_audio`] takes the position the
//! audio renderer reports and removes the drift, either by nudging the playback
//! rate (small drift, invisible) or by hard-snapping (large drift, e.g. after
//! the audio device stalled).
//!
//! Wall-clock time is inherently approximate, so this module is the one place
//! where a ratio is applied to real elapsed time. Content time is never
//! accumulated here: [`crate::render::plan_frame`] always recomputes the source
//! instant from a frame index, so error introduced by the clock can only ever
//! pick a different frame, never bend the timeline.

use crate::time::Rational;

/// Drift beyond this is corrected by snapping instead of slewing.
pub const DEFAULT_SLEW_LIMIT_US: i64 = 40_000;

/// How much wall time a slew correction is spread over.
const SLEW_WINDOW_US: i64 = 500_000;

#[derive(Clone, Debug)]
pub struct PlaybackClock {
    /// Timeline position committed when `anchor_wall_us` was taken.
    position_us: i64,
    anchor_wall_us: i64,
    playing: bool,
    /// Applied to wall time; 1 unless a slew is in progress.
    rate: Rational,
    slew_limit_us: i64,
}

impl Default for PlaybackClock {
    fn default() -> Self {
        Self::new(0)
    }
}

impl PlaybackClock {
    pub fn new(position_us: i64) -> Self {
        Self {
            position_us: position_us.max(0),
            anchor_wall_us: 0,
            playing: false,
            rate: Rational::ONE,
            slew_limit_us: DEFAULT_SLEW_LIMIT_US,
        }
    }

    pub fn set_slew_limit_us(&mut self, limit_us: i64) {
        self.slew_limit_us = limit_us.max(0);
    }

    pub fn is_playing(&self) -> bool {
        self.playing
    }

    /// Current timeline position, extrapolated from the anchor while playing.
    pub fn position_at(&self, now_wall_us: i64) -> i64 {
        if !self.playing {
            return self.position_us;
        }
        let elapsed = now_wall_us.saturating_sub(self.anchor_wall_us).max(0);
        self.position_us
            .saturating_add(Rational::scaled_us_to_us(elapsed, &self.rate))
            .max(0)
    }

    pub fn play(&mut self, now_wall_us: i64) {
        if self.playing {
            return;
        }
        self.anchor_wall_us = now_wall_us;
        self.playing = true;
        self.rate = Rational::ONE;
    }

    pub fn pause(&mut self, now_wall_us: i64) {
        if !self.playing {
            return;
        }
        self.position_us = self.position_at(now_wall_us);
        self.anchor_wall_us = now_wall_us;
        self.playing = false;
        self.rate = Rational::ONE;
    }

    pub fn seek(&mut self, position_us: i64, now_wall_us: i64) {
        self.position_us = position_us.max(0);
        self.anchor_wall_us = now_wall_us;
        self.rate = Rational::ONE;
    }

    /// Re-anchor to the authoritative audio position.
    ///
    /// Returns the drift that was observed, in microseconds, for logging. A
    /// positive value means audio was ahead of the video clock.
    pub fn sync_to_audio(&mut self, audio_position_us: i64, now_wall_us: i64) -> i64 {
        let predicted = self.position_at(now_wall_us);
        let drift = audio_position_us.saturating_sub(predicted);

        if drift == 0 {
            return 0;
        }

        if drift.abs() > self.slew_limit_us {
            // Too far gone to hide: jump. The listener hears a discontinuity
            // anyway, and a visible jump is better than permanent desync.
            self.position_us = audio_position_us.max(0);
            self.anchor_wall_us = now_wall_us;
            self.rate = Rational::ONE;
            return drift;
        }

        // Spread the correction over the next SLEW_WINDOW_US of wall time so
        // the video plays imperceptibly faster or slower for half a second.
        let window = SLEW_WINDOW_US;
        self.position_us = predicted;
        self.anchor_wall_us = now_wall_us;
        self.rate = Rational::new(window + drift, window)
            .unwrap_or(Rational::ONE)
            .clamp_ratio(
                Rational::new(98, 100).unwrap(),
                Rational::new(102, 100).unwrap(),
            );
        drift
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn paused_clock_does_not_advance() {
        let clock = PlaybackClock::new(1_000);
        assert_eq!(clock.position_at(0), 1_000);
        assert_eq!(clock.position_at(10_000_000), 1_000);
    }

    #[test]
    fn playing_clock_advances_with_wall_time() {
        let mut clock = PlaybackClock::new(0);
        clock.play(5_000);
        assert_eq!(clock.position_at(5_000), 0);
        assert_eq!(clock.position_at(1_005_000), 1_000_000);
    }

    #[test]
    fn clock_never_runs_backwards_when_wall_time_jumps_back() {
        let mut clock = PlaybackClock::new(0);
        clock.play(10_000_000);
        assert_eq!(clock.position_at(9_000_000), 0);
    }

    #[test]
    fn pause_commits_the_extrapolated_position() {
        let mut clock = PlaybackClock::new(0);
        clock.play(0);
        clock.pause(2_000_000);
        assert!(!clock.is_playing());
        assert_eq!(clock.position_at(99_000_000), 2_000_000);
    }

    #[test]
    fn seek_while_paused_moves_immediately() {
        let mut clock = PlaybackClock::new(0);
        clock.seek(7_000_000, 1_000);
        assert_eq!(clock.position_at(1_000), 7_000_000);
        assert_eq!(clock.position_at(5_000_000), 7_000_000);
    }

    #[test]
    fn seek_while_playing_restarts_from_the_new_position() {
        let mut clock = PlaybackClock::new(0);
        clock.play(0);
        clock.seek(3_000_000, 1_000_000);
        assert_eq!(clock.position_at(1_000_000), 3_000_000);
        assert_eq!(clock.position_at(1_500_000), 3_500_000);
    }

    #[test]
    fn large_drift_snaps_instead_of_slewing() {
        let mut clock = PlaybackClock::new(0);
        clock.play(0);
        // Video clock says 1 s, audio says 1.5 s: half a second behind.
        let drift = clock.sync_to_audio(1_500_000, 1_000_000);
        assert_eq!(drift, 500_000);
        assert_eq!(clock.position_at(1_000_000), 1_500_000);
        assert_eq!(clock.rate, Rational::ONE, "a snap resets the rate");
    }

    #[test]
    fn small_drift_slews_without_jumping() {
        let mut clock = PlaybackClock::new(0);
        clock.play(0);
        // 10 ms behind: below the slew limit, so no visible jump.
        let drift = clock.sync_to_audio(1_010_000, 1_000_000);
        assert_eq!(drift, 10_000);
        // Position is unchanged at the moment of sync...
        assert_eq!(clock.position_at(1_000_000), 1_000_000);
        // ...and the small rate excess recovers it within the window.
        assert!(clock.rate.to_f64() > 1.0);
        assert!(clock.rate.to_f64() <= 1.02);
        let after_window = clock.position_at(1_000_000 + SLEW_WINDOW_US);
        assert!(
            after_window >= 1_010_000,
            "drift recovered, got {after_window}"
        );
    }

    #[test]
    fn slew_rate_is_clamped() {
        let mut clock = PlaybackClock::new(0);
        clock.set_slew_limit_us(1_000_000);
        clock.play(0);
        clock.sync_to_audio(60_000, 0);
        assert!(clock.rate.to_f64() <= 1.02, "rate {}", clock.rate.to_f64());
    }

    #[test]
    fn zero_drift_is_a_no_op() {
        let mut clock = PlaybackClock::new(500_000);
        let drift = clock.sync_to_audio(500_000, 0);
        assert_eq!(drift, 0);
        assert_eq!(clock.rate, Rational::ONE);
    }
}
