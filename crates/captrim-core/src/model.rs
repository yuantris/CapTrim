//! Immutable-by-convention editing model.
//!
//! A `Timeline` is a plain serializable value. The [`crate::timeline::Editor`]
//! owns it and produces new snapshots per command, which gives undo/redo and
//! draft persistence for free.

use crate::error::{Error, Result};
use crate::media::MediaInfo;
use crate::time::Rational;
use serde::{Deserialize, Serialize};

pub type Id = u64;

pub const DRAFT_VERSION: u32 = 1;

#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum TrackKind {
    MainVideo,
    Overlay,
    Audio,
    Subtitle,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct Transform {
    pub scale: f32,
    pub dx: f32,
    pub dy: f32,
    pub rotation_deg: f32,
}

impl Default for Transform {
    fn default() -> Self {
        Self {
            scale: 1.0,
            dx: 0.0,
            dy: 0.0,
            rotation_deg: 0.0,
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct Clip {
    pub id: Id,
    pub source: Id,
    /// Source in/out points in source media microseconds.
    pub in_us: i64,
    pub out_us: i64,
    /// Position on the timeline, in timeline microseconds.
    pub start_us: i64,
    /// Playback speed multiplier. 2 means twice as fast.
    pub speed: Rational,
    pub volume: f32,
    pub transform: Transform,
    pub effects: Vec<String>,
}

impl Clip {
    pub fn new(source: Id, in_us: i64, out_us: i64, start_us: i64) -> Self {
        Self {
            id: 0,
            source,
            in_us,
            out_us,
            start_us,
            speed: Rational::ONE,
            volume: 1.0,
            transform: Transform::default(),
            effects: Vec::new(),
        }
    }

    /// Length of source material this clip consumes.
    pub fn source_duration_us(&self) -> i64 {
        self.out_us.saturating_sub(self.in_us)
    }

    /// Length occupied on the timeline: source duration divided by speed.
    pub fn duration_us(&self) -> i64 {
        let Some(inv) = self.speed.reciprocal() else {
            return 0;
        };
        Rational::scaled_us_to_us(self.source_duration_us(), &inv)
    }

    pub fn end_us(&self) -> i64 {
        self.start_us.saturating_add(self.duration_us())
    }

    pub fn contains(&self, t_us: i64) -> bool {
        t_us >= self.start_us && t_us < self.end_us()
    }

    /// Convert a timeline instant into a source-media instant.
    pub fn to_source_us(&self, t_us: i64) -> i64 {
        let elapsed = t_us.saturating_sub(self.start_us);
        self.in_us
            .saturating_add(Rational::scaled_us_to_us(elapsed, &self.speed))
    }

    pub fn validate(&self) -> Result<()> {
        if !self.speed.is_positive() {
            return Err(Error::invalid("clip speed must be positive"));
        }
        if self.out_us <= self.in_us {
            return Err(Error::invalid("clip out_us must be greater than in_us"));
        }
        if self.start_us < 0 {
            return Err(Error::invalid("clip start_us must not be negative"));
        }
        if !self.volume.is_finite() || self.volume < 0.0 {
            return Err(Error::invalid("clip volume must be finite and >= 0"));
        }
        if !self.transform.scale.is_finite() || self.transform.scale <= 0.0 {
            return Err(Error::invalid("clip scale must be finite and > 0"));
        }
        Ok(())
    }
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct Track {
    pub id: Id,
    pub kind: TrackKind,
    pub clips: Vec<Clip>,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct MediaSource {
    pub id: Id,
    pub uri: String,
    /// Filled in after probing. `None` is valid for a draft saved before probe.
    pub info: Option<MediaInfo>,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct Timeline {
    pub version: u32,
    pub width: u32,
    pub height: u32,
    pub frame_rate: Rational,
    pub media: Vec<MediaSource>,
    pub tracks: Vec<Track>,
}

impl Default for Timeline {
    fn default() -> Self {
        Self {
            version: DRAFT_VERSION,
            width: 1920,
            height: 1080,
            frame_rate: Rational::new(30, 1).unwrap(),
            media: Vec::new(),
            tracks: Vec::new(),
        }
    }
}

impl Timeline {
    /// Ids are derived, never persisted, so a draft can be merged safely.
    pub fn next_id(&self) -> Id {
        let mut max = 0u64;
        for m in &self.media {
            max = max.max(m.id);
        }
        for t in &self.tracks {
            max = max.max(t.id);
            for c in &t.clips {
                max = max.max(c.id);
            }
        }
        max.saturating_add(1)
    }

    pub fn duration_us(&self) -> i64 {
        self.tracks
            .iter()
            .flat_map(|t| t.clips.iter())
            .map(|c| c.end_us())
            .max()
            .unwrap_or(0)
    }

    /// All clips active at `t_us`, ordered from the bottom track upwards.
    pub fn clips_at(&self, t_us: i64) -> Vec<&Clip> {
        self.tracks
            .iter()
            .flat_map(|t| t.clips.iter())
            .filter(|c| c.contains(t_us))
            .collect()
    }

    pub fn find_clip(&self, id: Id) -> Option<(Id, &Clip)> {
        for t in &self.tracks {
            if let Some(c) = t.clips.iter().find(|c| c.id == id) {
                return Some((t.id, c));
            }
        }
        None
    }

    pub fn find_track_mut(&mut self, id: Id) -> Option<&mut Track> {
        self.tracks.iter_mut().find(|t| t.id == id)
    }

    pub fn media_mut(&mut self, id: Id) -> Option<&mut MediaSource> {
        self.media.iter_mut().find(|m| m.id == id)
    }

    pub fn to_draft_json(&self) -> Result<String> {
        serde_json::to_string(self).map_err(|e| Error::Internal(e.to_string()))
    }

    pub fn from_draft_json(json: &str) -> Result<Self> {
        let t: Self = serde_json::from_str(json)
            .map_err(|e| Error::invalid(format!("bad draft JSON: {e}")))?;
        t.validate()?;
        Ok(t)
    }

    pub fn validate(&self) -> Result<()> {
        if self.version > DRAFT_VERSION {
            return Err(Error::invalid(format!(
                "draft version {} is newer than supported {}",
                self.version, DRAFT_VERSION
            )));
        }
        if !self.frame_rate.is_positive() {
            return Err(Error::invalid("frame_rate must be positive"));
        }
        if self.width == 0 || self.height == 0 {
            return Err(Error::invalid("timeline dimensions must be non-zero"));
        }
        for t in &self.tracks {
            let mut ids = std::collections::HashSet::new();
            for c in &t.clips {
                c.validate()?;
                if !ids.insert(c.id) {
                    return Err(Error::invalid(format!("duplicate clip id {}", c.id)));
                }
                if !self.media.iter().any(|m| m.id == c.source) {
                    return Err(Error::invalid(format!(
                        "clip {} references unknown media {}",
                        c.id, c.source
                    )));
                }
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn speed_changes_timeline_length_not_source_length() {
        let mut c = Clip::new(1, 0, 10_000_000, 0);
        assert_eq!(c.duration_us(), 10_000_000);
        c.speed = Rational::new(2, 1).unwrap();
        assert_eq!(c.duration_us(), 5_000_000);
        assert_eq!(c.source_duration_us(), 10_000_000);
        c.speed = Rational::new(1, 2).unwrap();
        assert_eq!(c.duration_us(), 20_000_000);
    }

    #[test]
    fn timeline_instant_maps_back_to_source() {
        let mut c = Clip::new(1, 1_000_000, 5_000_000, 2_000_000);
        c.speed = Rational::new(2, 1).unwrap();
        assert_eq!(c.to_source_us(2_000_000), 1_000_000);
        assert_eq!(c.to_source_us(3_000_000), 3_000_000);
    }

    #[test]
    fn invalid_clips_are_rejected() {
        let c = Clip::new(1, 5_000_000, 1_000_000, 0);
        assert!(c.validate().is_err());
        let mut c = Clip::new(1, 0, 1_000_000, 0);
        c.speed = Rational::ZERO;
        assert!(c.validate().is_err());
    }

    #[test]
    fn draft_json_rejects_unknown_media_reference() {
        let mut t = Timeline::default();
        t.tracks.push(Track {
            id: 1,
            kind: TrackKind::MainVideo,
            clips: vec![Clip {
                id: 2,
                ..Clip::new(99, 0, 1_000_000, 0)
            }],
        });
        assert!(t.validate().is_err());
    }
}
