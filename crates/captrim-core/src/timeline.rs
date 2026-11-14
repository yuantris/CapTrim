//! Command-driven editing. Each command produces a new immutable snapshot,
//! which is the entire undo/redo mechanism.

use crate::error::{Error, Result};
use crate::model::{Clip, Id, Timeline, Track, TrackKind};
use crate::time::Rational;
use serde::{Deserialize, Serialize};

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum Command {
    AddMedia {
        uri: String,
    },
    AddTrack {
        kind: TrackKind,
    },
    InsertClip {
        track_id: Id,
        clip: Clip,
    },
    /// Split at a *timeline* instant; the right half gets a fresh id.
    SplitClip {
        clip_id: Id,
        at_us: i64,
    },
    RemoveClip {
        clip_id: Id,
    },
    MoveClip {
        clip_id: Id,
        to_track_id: Id,
        to_start_us: i64,
    },
    SetSpeed {
        clip_id: Id,
        speed: Rational,
    },
    SetVolume {
        clip_id: Id,
        volume: f32,
    },
    /// Trim the out point of a clip (source microseconds).
    SetOutPoint {
        clip_id: Id,
        out_us: i64,
    },
    SetCanvas {
        width: u32,
        height: u32,
        frame_rate: Rational,
    },
    RemoveTrack {
        track_id: Id,
    },
}

fn overlaps(a: &Clip, b: &Clip) -> bool {
    a.start_us < b.end_us() && b.start_us < a.end_us()
}

impl Timeline {
    /// Apply one command in place. Returns the id of any newly created object.
    pub fn apply(&mut self, cmd: &Command) -> Result<Option<Id>> {
        match cmd {
            Command::AddMedia { uri } => {
                if uri.is_empty() {
                    return Err(Error::invalid("uri must not be empty"));
                }
                if let Some(m) = self.media.iter().find(|m| m.uri == *uri) {
                    return Ok(Some(m.id));
                }
                let id = self.next_id();
                self.media.push(crate::model::MediaSource {
                    id,
                    uri: uri.clone(),
                    info: None,
                });
                Ok(Some(id))
            }

            Command::AddTrack { kind } => {
                let id = self.next_id();
                self.tracks.push(Track {
                    id,
                    kind: *kind,
                    clips: Vec::new(),
                });
                Ok(Some(id))
            }

            Command::InsertClip { track_id, clip } => {
                let mut clip = clip.clone();
                if clip.id == 0 {
                    clip.id = self.next_id();
                }
                clip.validate()?;
                if !self.media.iter().any(|m| m.id == clip.source) {
                    return Err(Error::not_found(format!("media {}", clip.source)));
                }
                let track = self
                    .find_track_mut(*track_id)
                    .ok_or_else(|| Error::not_found(format!("track {track_id}")))?;
                if track.clips.iter().any(|c| overlaps(c, &clip)) {
                    return Err(Error::OutOfRange(
                        "clip overlaps an existing clip on this track".into(),
                    ));
                }
                let id = clip.id;
                track.clips.push(clip);
                track.clips.sort_by_key(|c| c.start_us);
                Ok(Some(id))
            }

            Command::SplitClip { clip_id, at_us } => {
                let track_id = self
                    .find_clip(*clip_id)
                    .map(|(id, _)| id)
                    .ok_or_else(|| Error::not_found(format!("clip {clip_id}")))?;
                let new_id = self.next_id();
                let track = self.find_track_mut(track_id).unwrap();
                let idx = track.clips.iter().position(|c| c.id == *clip_id).unwrap();
                let original = track.clips[idx].clone();
                if *at_us <= original.start_us || *at_us >= original.end_us() {
                    return Err(Error::OutOfRange(format!(
                        "split point {at_us} is outside clip [{}, {})",
                        original.start_us,
                        original.end_us()
                    )));
                }
                let source_at = original.to_source_us(*at_us);
                let mut right = original.clone();
                right.id = new_id;
                right.in_us = source_at;
                right.start_us = *at_us;
                track.clips[idx].out_us = source_at;
                track.clips.insert(idx + 1, right.clone());
                Ok(Some(right.id))
            }

            Command::RemoveClip { clip_id } => {
                for t in &mut self.tracks {
                    if let Some(idx) = t.clips.iter().position(|c| c.id == *clip_id) {
                        t.clips.remove(idx);
                        return Ok(None);
                    }
                }
                Err(Error::not_found(format!("clip {clip_id}")))
            }

            Command::MoveClip {
                clip_id,
                to_track_id,
                to_start_us,
            } => {
                let mut moved: Option<Clip> = None;
                for t in &mut self.tracks {
                    if let Some(idx) = t.clips.iter().position(|c| c.id == *clip_id) {
                        moved = Some(t.clips.remove(idx));
                        break;
                    }
                }
                let mut clip = moved.ok_or_else(|| Error::not_found(format!("clip {clip_id}")))?;
                clip.start_us = *to_start_us;
                clip.validate()?;
                let track = self
                    .find_track_mut(*to_track_id)
                    .ok_or_else(|| Error::not_found(format!("track {to_track_id}")))?;
                if track.clips.iter().any(|c| overlaps(c, &clip)) {
                    return Err(Error::OutOfRange(
                        "clip would overlap an existing clip".into(),
                    ));
                }
                track.clips.push(clip);
                track.clips.sort_by_key(|c| c.start_us);
                Ok(None)
            }

            Command::SetSpeed { clip_id, speed } => {
                let clip = find_clip_mut(self, *clip_id)?;
                clip.speed = *speed;
                clip.validate()?;
                Ok(None)
            }

            Command::SetVolume { clip_id, volume } => {
                let clip = find_clip_mut(self, *clip_id)?;
                clip.volume = *volume;
                clip.validate()?;
                Ok(None)
            }

            Command::SetOutPoint { clip_id, out_us } => {
                let clip = find_clip_mut(self, *clip_id)?;
                clip.out_us = *out_us;
                clip.validate()?;
                Ok(None)
            }

            Command::SetCanvas {
                width,
                height,
                frame_rate,
            } => {
                if *width == 0 || *height == 0 || !frame_rate.is_positive() {
                    return Err(Error::invalid("invalid canvas settings"));
                }
                self.width = *width;
                self.height = *height;
                self.frame_rate = *frame_rate;
                Ok(None)
            }

            Command::RemoveTrack { track_id } => {
                let before = self.tracks.len();
                self.tracks.retain(|t| t.id != *track_id);
                if self.tracks.len() == before {
                    return Err(Error::not_found(format!("track {track_id}")));
                }
                Ok(None)
            }
        }
    }
}

fn find_clip_mut(t: &mut Timeline, id: Id) -> Result<&mut Clip> {
    for track in &mut t.tracks {
        if let Some(c) = track.clips.iter_mut().find(|c| c.id == id) {
            return Ok(c);
        }
    }
    Err(Error::not_found(format!("clip {id}")))
}

pub struct Editor {
    timeline: Timeline,
    undo: Vec<Timeline>,
    redo: Vec<Timeline>,
    limit: usize,
    /// Bumped on every change, including the silent metadata updates that do
    /// not create an undo step. A preview session compares this to know whether
    /// the frame on screen is stale.
    revision: u64,
}

impl Default for Editor {
    fn default() -> Self {
        Self::new()
    }
}

impl Editor {
    pub fn new() -> Self {
        Self {
            timeline: Timeline::default(),
            undo: Vec::new(),
            redo: Vec::new(),
            limit: 100,
            revision: 0,
        }
    }

    pub fn from_draft(json: &str) -> Result<Self> {
        Ok(Self {
            timeline: Timeline::from_draft_json(json)?,
            undo: Vec::new(),
            redo: Vec::new(),
            limit: 100,
            revision: 0,
        })
    }

    pub fn from_timeline(timeline: Timeline) -> Result<Self> {
        timeline.validate()?;
        Ok(Self {
            timeline,
            undo: Vec::new(),
            redo: Vec::new(),
            limit: 100,
            revision: 0,
        })
    }

    pub fn timeline(&self) -> &Timeline {
        &self.timeline
    }

    /// Replace timeline metadata without creating an undo step. Probe results
    /// are derived data, not user edits, so undo must not revert them.
    pub fn set_timeline_silent(&mut self, timeline: Timeline) -> Result<()> {
        timeline.validate()?;
        self.timeline = timeline;
        self.revision = self.revision.wrapping_add(1);
        Ok(())
    }

    /// Monotonic change counter. A preview session compares it to decide
    /// whether the frame currently on screen is stale.
    pub fn revision(&self) -> u64 {
        self.revision
    }

    pub fn draft_json(&self) -> Result<String> {
        self.timeline.to_draft_json()
    }

    pub fn can_undo(&self) -> bool {
        !self.undo.is_empty()
    }

    pub fn can_redo(&self) -> bool {
        !self.redo.is_empty()
    }

    pub fn apply(&mut self, cmd: &Command) -> Result<Option<Id>> {
        let mut candidate = self.timeline.clone();
        let created = candidate.apply(cmd)?;
        candidate.validate()?;
        let previous = std::mem::replace(&mut self.timeline, candidate);
        self.undo.push(previous);
        if self.undo.len() > self.limit {
            self.undo.remove(0);
        }
        self.redo.clear();
        self.revision = self.revision.wrapping_add(1);
        Ok(created)
    }

    pub fn apply_json(&mut self, json: &str) -> Result<Option<Id>> {
        let cmd: Command = serde_json::from_str(json)
            .map_err(|e| Error::invalid(format!("bad command JSON: {e}")))?;
        self.apply(&cmd)
    }

    pub fn undo(&mut self) -> bool {
        let Some(previous) = self.undo.pop() else {
            return false;
        };
        let current = std::mem::replace(&mut self.timeline, previous);
        self.redo.push(current);
        self.revision = self.revision.wrapping_add(1);
        true
    }

    pub fn redo(&mut self) -> bool {
        let Some(next) = self.redo.pop() else {
            return false;
        };
        let current = std::mem::replace(&mut self.timeline, next);
        self.undo.push(current);
        self.revision = self.revision.wrapping_add(1);
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::DRAFT_VERSION;

    fn seeded() -> (Editor, Id, Id) {
        let mut e = Editor::new();
        let media = e
            .apply(&Command::AddMedia {
                uri: "file:///clip.mp4".into(),
            })
            .unwrap()
            .unwrap();
        let track = e
            .apply(&Command::AddTrack {
                kind: TrackKind::MainVideo,
            })
            .unwrap()
            .unwrap();
        (e, media, track)
    }

    #[test]
    fn split_keeps_total_duration_and_is_undoable() {
        let (mut e, media, track) = seeded();
        let clip = Clip::new(media, 0, 10_000_000, 0);
        let clip_id = e
            .apply(&Command::InsertClip {
                track_id: track,
                clip,
            })
            .unwrap()
            .unwrap();
        assert_eq!(e.timeline().duration_us(), 10_000_000);

        let right = e
            .apply(&Command::SplitClip {
                clip_id,
                at_us: 4_000_000,
            })
            .unwrap()
            .unwrap();
        assert_ne!(clip_id, right);
        assert_eq!(e.timeline().tracks[0].clips.len(), 2);
        assert_eq!(e.timeline().duration_us(), 10_000_000);
        let parts = &e.timeline().tracks[0].clips;
        assert_eq!(parts[0].out_us, 4_000_000);
        assert_eq!(parts[1].in_us, 4_000_000);
        assert_eq!(parts[1].start_us, 4_000_000);

        assert!(e.undo());
        assert_eq!(e.timeline().tracks[0].clips.len(), 1);
        assert!(e.redo());
        assert_eq!(e.timeline().tracks[0].clips.len(), 2);
    }

    #[test]
    fn split_with_speed_maps_source_time_correctly() {
        let (mut e, media, track) = seeded();
        let mut clip = Clip::new(media, 0, 10_000_000, 0);
        clip.speed = Rational::new(2, 1).unwrap();
        let clip_id = e
            .apply(&Command::InsertClip {
                track_id: track,
                clip,
            })
            .unwrap()
            .unwrap();
        // timeline 2s == source 4s at 2x
        e.apply(&Command::SplitClip {
            clip_id,
            at_us: 2_000_000,
        })
        .unwrap();
        let parts = &e.timeline().tracks[0].clips;
        assert_eq!(parts[0].out_us, 4_000_000);
        assert_eq!(parts[1].in_us, 4_000_000);
        assert_eq!(parts[1].start_us, 2_000_000);
    }

    #[test]
    fn overlapping_clips_are_rejected_and_do_not_mutate() {
        let (mut e, media, track) = seeded();
        e.apply(&Command::InsertClip {
            track_id: track,
            clip: Clip::new(media, 0, 5_000_000, 0),
        })
        .unwrap();
        let before = e.timeline().clone();
        let err = e.apply(&Command::InsertClip {
            track_id: track,
            clip: Clip::new(media, 0, 5_000_000, 1_000_000),
        });
        assert!(err.is_err());
        assert_eq!(*e.timeline(), before);
    }

    #[test]
    fn command_json_is_the_public_bridge_format() {
        let (mut e, _, _) = seeded();
        e.apply_json(r#"{"type":"set_canvas","width":1280,"height":720,"frame_rate":{"num":30000,"den":1001}}"#)
            .unwrap();
        assert_eq!(e.timeline().width, 1280);
        assert_eq!(
            e.timeline().frame_rate,
            Rational::new(30_000, 1001).unwrap()
        );
    }

    #[test]
    fn draft_json_round_trips_through_editor() {
        let (mut e, media, track) = seeded();
        e.apply(&Command::InsertClip {
            track_id: track,
            clip: Clip::new(media, 0, 3_000_000, 0),
        })
        .unwrap();
        let json = e.draft_json().unwrap();
        let restored = Editor::from_draft(&json).unwrap();
        assert_eq!(restored.timeline(), e.timeline());
        assert_eq!(restored.timeline().version, DRAFT_VERSION);
    }
}
