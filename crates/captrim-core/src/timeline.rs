//! Command-driven editing. Each command produces a new immutable snapshot,
//! which is the entire undo/redo mechanism.

use crate::error::{Error, Result};
use crate::model::{Clip, Id, Timeline, Track, TrackKind, Transition};
use crate::time::Rational;
use serde::{Deserialize, Serialize};

/// Where a freshly inserted subtitle sits: `dy = 0.36` puts the block's centre
/// just below the middle, i.e. in the lower third of the canvas. `dy` is in
/// canvas-normalised units, 1.0 being a full canvas height down from the centre.
pub const SUBTITLE_DEFAULT_DY: f32 = 0.36;

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
    /// Reposition a clip on the canvas. See [`crate::model::Transform`] for the
    /// units; the preview compositor and the export path both read them.
    SetTransform {
        clip_id: Id,
        transform: crate::model::Transform,
    },
    /// Attach (`Some`) or clear (`None`) the dissolve at a clip's start.
    /// The cut does not have to have a predecessor yet — a lone clip simply
    /// fades in from black, and a later neighbour turns it into a dissolve.
    SetTransition {
        clip_id: Id,
        transition: Option<Transition>,
    },
    /// Replace a clip's effect list outright. Names must come from the v1
    /// vocabulary ([`crate::effects::CANONICAL_ORDER`]); `Clip::validate`
    /// rejects anything else so an unknown name never reaches a backend.
    /// The list is a set: order and duplicates carry no meaning.
    SetEffects {
        clip_id: Id,
        effects: Vec<String>,
    },
    /// Insert a text clip on a subtitle track. Text clips have no media source
    /// (`source` stays 0); their timeline length is carried by `duration_us`
    /// via `in_us = 0, out_us = duration_us`, so all the existing duration and
    /// overlap arithmetic applies unchanged. Placement uses the ordinary
    /// `SetTransform` command rather than a second, text-specific one.
    InsertTextClip {
        track_id: Id,
        text: String,
        start_us: i64,
        duration_us: i64,
    },
    /// Replace the text of an existing subtitle clip in place.
    SetText {
        clip_id: Id,
        text: String,
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
                // A dissolve belongs to the cut at the clip's start: the right
                // half opens a fresh cut of its own, and the left half keeps
                // the window only while it still has room for it. Splitting
                // inside the window would leave a fade longer than the half
                // that owns it, so the dissolve is dropped rather than
                // failing the whole split.
                right.transition_in = None;
                if let Some(transition) = &original.transition_in {
                    if *at_us <= original.start_us.saturating_add(transition.duration_us) {
                        track.clips[idx].transition_in = None;
                    }
                }
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

            Command::SetTransform { clip_id, transform } => {
                if !transform.scale.is_finite() || transform.scale <= 0.0 {
                    return Err(Error::invalid("transform scale must be positive"));
                }
                if !transform.dx.is_finite()
                    || !transform.dy.is_finite()
                    || !transform.rotation_deg.is_finite()
                {
                    return Err(Error::invalid("transform must be finite"));
                }
                let clip = find_clip_mut(self, *clip_id)?;
                clip.transform = transform.clone();
                clip.validate()?;
                Ok(None)
            }

            Command::SetTransition {
                clip_id,
                transition,
            } => {
                let clip = find_clip_mut(self, *clip_id)?;
                clip.transition_in = *transition;
                clip.validate()?;
                Ok(None)
            }

            Command::SetEffects { clip_id, effects } => {
                let clip = find_clip_mut(self, *clip_id)?;
                clip.effects = effects.clone();
                clip.validate()?;
                Ok(None)
            }

            Command::InsertTextClip {
                track_id,
                text,
                start_us,
                duration_us,
            } => {
                if text.trim().is_empty() {
                    return Err(Error::invalid("subtitle text must not be blank"));
                }
                if *duration_us <= 0 {
                    return Err(Error::invalid("subtitle duration must be positive"));
                }
                let id = self.next_id();
                let track = self
                    .find_track_mut(*track_id)
                    .ok_or_else(|| Error::not_found(format!("track {track_id}")))?;
                if track.kind != TrackKind::Subtitle {
                    return Err(Error::invalid("InsertTextClip requires a subtitle track"));
                }
                let mut clip = Clip::new(0, 0, *duration_us, *start_us);
                clip.id = id;
                clip.text = Some(text.clone());
                // Subtitles land in the lower third, not dead centre: a new
                // caption covering the subject's face is the one placement
                // nobody wants. `dx` stays 0, so it is centred horizontally.
                // The editor moves it with the ordinary SetTransform command.
                clip.transform.dy = SUBTITLE_DEFAULT_DY;
                clip.validate()?;
                if track.clips.iter().any(|c| overlaps(c, &clip)) {
                    return Err(Error::OutOfRange(
                        "subtitle overlaps an existing clip on this track".into(),
                    ));
                }
                track.clips.push(clip);
                track.clips.sort_by_key(|c| c.start_us);
                Ok(Some(id))
            }

            Command::SetText { clip_id, text } => {
                if text.trim().is_empty() {
                    return Err(Error::invalid("subtitle text must not be blank"));
                }
                let (_, clip) = self
                    .find_clip(*clip_id)
                    .ok_or_else(|| Error::not_found(format!("clip {clip_id}")))?;
                if clip.text.is_none() {
                    return Err(Error::invalid("SetText requires a subtitle clip"));
                }
                let clip = find_clip_mut(self, *clip_id)?;
                clip.text = Some(text.clone());
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
    fn set_transform_reaches_the_frame_plan() {
        use crate::model::Transform;
        let (mut e, media, track) = seeded();
        let clip_id = e
            .apply(&Command::InsertClip {
                track_id: track,
                clip: Clip::new(media, 0, 4_000_000, 0),
            })
            .unwrap()
            .unwrap();
        e.apply(&Command::SetTransform {
            clip_id,
            transform: Transform {
                scale: 0.5,
                dx: 0.25,
                dy: -0.5,
                rotation_deg: 90.0,
            },
        })
        .unwrap();

        // The plan is what the compositor draws, so the check belongs there
        // rather than on the clip that was written.
        let plan = crate::render::plan_frame(e.timeline(), 1_000_000);
        let layer = &plan.layers[0];
        assert_eq!(layer.transform.scale, 0.5);
        assert_eq!(layer.transform.dx, 0.25);
        assert_eq!(layer.transform.dy, -0.5);
        assert_eq!(layer.transform.rotation_deg, 90.0);
    }

    #[test]
    fn set_transform_rejects_degenerate_values() {
        use crate::model::Transform;
        let (mut e, media, track) = seeded();
        let clip_id = e
            .apply(&Command::InsertClip {
                track_id: track,
                clip: Clip::new(media, 0, 4_000_000, 0),
            })
            .unwrap()
            .unwrap();
        let before = e.timeline().clone();
        // A zero scale would make every vertex of the layer coincide, which the
        // compositor draws as nothing at all -- an invisible clip with no error
        // is worse than a rejected edit.
        let err = e.apply(&Command::SetTransform {
            clip_id,
            transform: Transform {
                scale: 0.0,
                ..Transform::default()
            },
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

    #[test]
    fn set_transition_attaches_clears_and_rejects_bad_windows() {
        use crate::model::{Transition, TransitionKind};
        let (mut e, media, track) = seeded();
        let clip_id = e
            .apply(&Command::InsertClip {
                track_id: track,
                clip: Clip::new(media, 0, 4_000_000, 0),
            })
            .unwrap()
            .unwrap();
        let fade = Transition {
            kind: TransitionKind::Fade,
            duration_us: 1_000_000,
        };
        e.apply(&Command::SetTransition {
            clip_id,
            transition: Some(fade),
        })
        .unwrap();
        assert_eq!(e.timeline().tracks[0].clips[0].transition_in, Some(fade));

        // The JSON bridge is the format platforms actually send.
        e.apply_json(&format!(
            r#"{{"type":"set_transition","clip_id":{clip_id},"transition":null}}"#
        ))
        .unwrap();
        assert!(e.timeline().tracks[0].clips[0].transition_in.is_none());
        assert!(e.undo(), "clearing a transition is an undoable step");
        assert_eq!(e.timeline().tracks[0].clips[0].transition_in, Some(fade));

        // A window longer than the clip it sits on would push the ramp past
        // the clip's own end, so it is rejected without mutating the draft.
        let before = e.timeline().clone();
        let err = e.apply(&Command::SetTransition {
            clip_id,
            transition: Some(Transition {
                kind: TransitionKind::Fade,
                duration_us: 9_000_000,
            }),
        });
        assert!(err.is_err());
        assert_eq!(*e.timeline(), before);
    }

    #[test]
    fn split_keeps_a_dissolve_only_while_the_left_half_still_fits_it() {
        use crate::model::{Transition, TransitionKind};
        let (mut e, media, track) = seeded();
        let clip_id = e
            .apply(&Command::InsertClip {
                track_id: track,
                clip: Clip::new(media, 0, 6_000_000, 0),
            })
            .unwrap()
            .unwrap();
        e.apply(&Command::SetTransition {
            clip_id,
            transition: Some(Transition {
                kind: TransitionKind::Fade,
                duration_us: 1_000_000,
            }),
        })
        .unwrap();

        // Split after the window: the left half still owns the whole dissolve.
        let right = e
            .apply(&Command::SplitClip {
                clip_id,
                at_us: 5_000_000,
            })
            .unwrap()
            .unwrap();
        let parts = &e.timeline().tracks[0].clips;
        assert!(parts[0].transition_in.is_some());
        assert!(
            parts[1].transition_in.is_none(),
            "right half opens a new cut"
        );
        assert_eq!(parts[1].id, right);

        // Split inside the window: the left half would end up shorter than
        // its own fade, so the dissolve is dropped rather than failing the
        // whole split.
        e.apply(&Command::SplitClip {
            clip_id,
            at_us: 500_000,
        })
        .unwrap();
        let parts = &e.timeline().tracks[0].clips;
        assert_eq!(parts.len(), 3);
        assert!(parts[0].transition_in.is_none());
        assert!(parts[1].transition_in.is_none());
        assert!(parts[2].transition_in.is_none());
        assert_eq!(e.timeline().duration_us(), 6_000_000);
    }

    #[test]
    fn set_effects_updates_the_list_and_rejects_unknown_names() {
        let (mut e, media, track) = seeded();
        let clip_id = e
            .apply(&Command::InsertClip {
                track_id: track,
                clip: Clip::new(media, 0, 3_000_000, 0),
            })
            .unwrap()
            .unwrap();

        // The JSON bridge is the format platforms actually send.
        e.apply_json(&format!(
            r#"{{"type":"set_effects","clip_id":{clip_id},"effects":["blur","hflip"]}}"#
        ))
        .unwrap();
        assert_eq!(
            e.timeline().tracks[0].clips[0].effects,
            vec!["blur".to_string(), "hflip".to_string()]
        );

        // Clearing the list is an ordinary undoable step.
        e.apply_json(&format!(
            r#"{{"type":"set_effects","clip_id":{clip_id},"effects":[]}}"#
        ))
        .unwrap();
        assert!(e.timeline().tracks[0].clips[0].effects.is_empty());
        assert!(e.undo(), "clearing effects is an undoable step");
        assert_eq!(e.timeline().tracks[0].clips[0].effects.len(), 2);

        // An unknown name would reach a backend that cannot render it, so it
        // is rejected without mutating the draft.
        let before = e.timeline().clone();
        let err = e.apply(&Command::SetEffects {
            clip_id,
            effects: vec!["sepia".to_string()],
        });
        assert!(err.is_err());
        assert_eq!(*e.timeline(), before);
    }

    #[test]
    fn insert_text_clip_needs_a_subtitle_track_and_is_undoable() {
        let mut e = Editor::new();
        let video_track = e
            .apply(&Command::AddTrack {
                kind: TrackKind::MainVideo,
            })
            .unwrap()
            .unwrap();
        assert!(
            e.apply(&Command::InsertTextClip {
                track_id: video_track,
                text: "wrong track".into(),
                start_us: 0,
                duration_us: 1_000_000,
            })
            .is_err(),
            "text clips belong on a subtitle track"
        );

        let track = e
            .apply(&Command::AddTrack {
                kind: TrackKind::Subtitle,
            })
            .unwrap()
            .unwrap();
        let id = e
            .apply(&Command::InsertTextClip {
                track_id: track,
                text: "第一行".into(),
                start_us: 1_000_000,
                duration_us: 2_000_000,
            })
            .unwrap()
            .unwrap();
        let clip = e.timeline().find_clip(id).unwrap().1;
        assert_eq!(clip.text.as_deref(), Some("第一行"));
        assert_eq!(clip.source, 0, "text clips have no media source");
        assert_eq!(clip.duration_us(), 2_000_000);
        assert_eq!(e.timeline().duration_us(), 3_000_000);

        assert!(e.undo());
        assert!(e.timeline().find_clip(id).is_none());
        assert!(e.redo());
        assert!(e.timeline().find_clip(id).is_some());
    }

    #[test]
    fn set_text_updates_only_subtitle_clips_and_rejects_blank() {
        let mut e = Editor::new();
        let track = e
            .apply(&Command::AddTrack {
                kind: TrackKind::Subtitle,
            })
            .unwrap()
            .unwrap();
        let id = e
            .apply(&Command::InsertTextClip {
                track_id: track,
                text: "old".into(),
                start_us: 0,
                duration_us: 1_000_000,
            })
            .unwrap()
            .unwrap();

        assert!(e
            .apply(&Command::SetText {
                clip_id: id,
                text: "   ".into(),
            })
            .is_err());
        e.apply(&Command::SetText {
            clip_id: id,
            text: "new".into(),
        })
        .unwrap();
        assert_eq!(
            e.timeline().find_clip(id).unwrap().1.text.as_deref(),
            Some("new")
        );

        let media_id = e
            .apply(&Command::AddMedia {
                uri: "file:///v.mp4".into(),
            })
            .unwrap()
            .unwrap();
        let video_track = e
            .apply(&Command::AddTrack {
                kind: TrackKind::MainVideo,
            })
            .unwrap()
            .unwrap();
        let media_clip = e
            .apply(&Command::InsertClip {
                track_id: video_track,
                clip: Clip::new(media_id, 0, 1_000_000, 0),
            })
            .unwrap()
            .unwrap();
        assert!(e
            .apply(&Command::SetText {
                clip_id: media_clip,
                text: "not allowed".into(),
            })
            .is_err());
    }

    #[test]
    fn text_clips_overlap_check_uses_the_same_rules_as_media_clips() {
        let mut e = Editor::new();
        let track = e
            .apply(&Command::AddTrack {
                kind: TrackKind::Subtitle,
            })
            .unwrap()
            .unwrap();
        e.apply(&Command::InsertTextClip {
            track_id: track,
            text: "a".into(),
            start_us: 1_000_000,
            duration_us: 2_000_000,
        })
        .unwrap();
        assert!(
            e.apply(&Command::InsertTextClip {
                track_id: track,
                text: "b".into(),
                start_us: 2_000_000,
                duration_us: 2_000_000,
            })
            .is_err(),
            "overlapping subtitles are rejected"
        );
        e.apply(&Command::InsertTextClip {
            track_id: track,
            text: "b".into(),
            start_us: 3_000_000,
            duration_us: 2_000_000,
        })
        .unwrap();
    }
}
