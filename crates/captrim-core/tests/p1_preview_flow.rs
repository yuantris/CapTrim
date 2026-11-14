//! P1 invariant: the preview loop and an export walk must produce the same
//! frames from the same draft.
//!
//! This is the test that makes the architecture worth its complexity. If it
//! ever fails, the user will see one picture in the editor and get another in
//! the exported file -- the single most expensive class of bug in a video
//! editor, because it is invisible until someone watches the whole render.

use captrim_core::preview::PreviewSession;
use captrim_core::render::{plan_frame, FramePlan};
use captrim_core::{Command, Editor, Step};

fn apply(editor: &mut Editor, cmd: &Command) -> u64 {
    editor
        .apply(cmd)
        .expect("command rejected")
        .expect("command created no id")
}

fn editor_with_clips() -> Editor {
    let mut editor = Editor::new();
    let video_id = apply(
        &mut editor,
        &Command::AddMedia {
            uri: "file:///a.mp4".into(),
        },
    );
    let music_id = apply(
        &mut editor,
        &Command::AddMedia {
            uri: "file:///b.m4a".into(),
        },
    );

    let track = apply(
        &mut editor,
        &Command::AddTrack {
            kind: captrim_core::TrackKind::MainVideo,
        },
    );
    let audio_track = apply(
        &mut editor,
        &Command::AddTrack {
            kind: captrim_core::TrackKind::Audio,
        },
    );

    // 4 s clip, then split it so the draft has two adjacent clips.
    let clip = apply(
        &mut editor,
        &Command::InsertClip {
            track_id: track,
            clip: captrim_core::Clip::new(video_id, 0, 4_000_000, 0),
        },
    );
    editor
        .apply(&Command::SplitClip {
            clip_id: clip,
            at_us: 2_000_000,
        })
        .expect("split rejected");
    // Speed up the right half: 2 s of source over 1 s of timeline.
    let right = editor.timeline().tracks[0]
        .clips
        .iter()
        .find(|c| c.start_us == 2_000_000)
        .expect("right half missing")
        .id;
    editor
        .apply(&Command::SetSpeed {
            clip_id: right,
            speed: captrim_core::Rational::new(2, 1).unwrap(),
        })
        .expect("speed rejected");

    let music_clip = apply(
        &mut editor,
        &Command::InsertClip {
            track_id: audio_track,
            clip: captrim_core::Clip::new(music_id, 1_000_000, 4_000_000, 0),
        },
    );
    editor
        .apply(&Command::SetVolume {
            clip_id: music_clip,
            volume: 0.4,
        })
        .expect("volume rejected");

    editor
}

/// The export path: walk every frame index in order, plan it by index, never
/// skip. Using the index (not a timestamp) is what keeps NTSC frame boundaries
/// exact.
fn export_walk(timeline: &captrim_core::Timeline) -> Vec<FramePlan> {
    (0..timeline.frame_count())
        .map(|i| timeline.plan_frame_at(i))
        .collect()
}

/// The preview path: run the real render loop as fast as it is due.
fn preview_walk(timeline: &captrim_core::Timeline) -> Vec<FramePlan> {
    let mut session = PreviewSession::new(timeline.clone());
    session.set_playing(true, 0);

    let mut frames = Vec::new();
    let mut now = 0i64;
    loop {
        match session.step(now) {
            Step::Present { frame } => frames.push(frame),
            Step::WaitFrame { due_wall_us } => {
                assert!(
                    due_wall_us > now,
                    "WaitFrame must move time forward: {due_wall_us} <= {now}"
                );
                now = due_wall_us;
            }
            Step::Idle => break,
            Step::End { .. } => break,
        }
        assert!(frames.len() <= 10_000, "render loop did not terminate");
    }
    frames
}

#[test]
fn preview_and_export_produce_identical_frames() {
    let editor = editor_with_clips();
    let timeline = editor.timeline();

    let exported = export_walk(timeline);
    let previewed = preview_walk(timeline);

    assert!(!exported.is_empty());
    assert_eq!(
        exported.len(),
        previewed.len(),
        "preview dropped or added frames: export={} preview={}",
        exported.len(),
        previewed.len()
    );

    for (index, (a, b)) in exported.iter().zip(previewed.iter()).enumerate() {
        assert_eq!(a.frame_index, index as u64, "export frame order");
        assert_eq!(b.frame_index, index as u64, "preview frame order");
        assert_eq!(a, b, "frame {index} differs between export and preview");
    }
}

#[test]
fn every_frame_maps_to_the_expected_source_instant() {
    let editor = editor_with_clips();
    let timeline = editor.timeline();

    // Left half: 1:1 mapping from timeline to source.
    let plan = plan_frame(timeline, 1_000_000);
    assert_eq!(plan.layers[0].source_us, 1_000_000);

    // Right half starts at 2 s on the timeline and runs at 2x, so 500 ms into
    // it we are 1 s into the source segment that began at 2 s.
    let plan = plan_frame(timeline, 2_500_000);
    assert_eq!(plan.layers[0].source_us, 3_000_000);

    // The audio clip started at source 1 s, so timeline 0 is source 1 s.
    let plan = plan_frame(timeline, 0);
    let music = plan
        .audio
        .iter()
        .find(|a| a.uri.ends_with("b.m4a"))
        .expect("music missing from the mix");
    assert_eq!(music.source_us, 1_000_000);
    assert_eq!(music.volume, 0.4);
    // The video clip contributes sound too, at full volume.
    assert_eq!(plan.audio.len(), 2);
}

#[test]
fn scrubbing_lands_on_the_frame_under_the_playhead() {
    let editor = editor_with_clips();
    let timeline = editor.timeline();
    let mut session = PreviewSession::new(timeline.clone());

    for target_us in [0i64, 1, 999_999, 2_000_000, 2_999_999] {
        session.seek(target_us, 0);
        let frame = match session.step(0) {
            Step::Present { frame } => frame,
            other => panic!("seek to {target_us} produced {other:?}"),
        };
        let expected = plan_frame(timeline, target_us);
        assert_eq!(frame, expected, "seek to {target_us}");
        assert_eq!(frame.frame_index, timeline.frame_index_at(target_us));
    }
}

#[test]
fn a_slow_render_loop_drops_frames_but_keeps_time() {
    let editor = editor_with_clips();
    let timeline = editor.timeline();
    let mut session = PreviewSession::new(timeline.clone());
    session.set_playing(true, 0);

    // Frame 0 on time, then the loop stalls for 100 ms before asking again.
    assert!(matches!(session.step(0), Step::Present { .. }));
    let frame = match session.step(100_000) {
        Step::Present { frame } => frame,
        other => panic!("expected a present after the stall, got {other:?}"),
    };

    // Time did not slow down to accommodate the slow device: it jumped to the
    // frame the clock is actually on, and reports what it skipped.
    assert_eq!(frame.frame_index, timeline.frame_index_at(100_000));
    assert_eq!(session.dropped_frames(), frame.frame_index - 1);
}

#[test]
fn pausing_and_resuming_keeps_the_position() {
    let editor = editor_with_clips();
    let mut session = PreviewSession::new(editor.timeline().clone());

    session.set_playing(true, 0);
    session.step(0);
    session.step(500_000); // 15 frames in

    session.set_playing(false, 500_000);
    assert_eq!(
        session.position_at(2_000_000),
        500_000,
        "paused clock is frozen"
    );

    session.set_playing(true, 2_000_000);
    assert_eq!(
        session.position_at(2_000_000),
        500_000,
        "resume keeps position"
    );

    // Frame 15 is still on screen, so the first step after resuming is a wait,
    // not a redraw. The next frame boundary produces frame 16.
    assert!(matches!(session.step(2_000_000), Step::WaitFrame { .. }));
    match session.step(2_040_000) {
        Step::Present { frame } => assert_eq!(frame.frame_index, 16),
        other => panic!("expected Present, got {other:?}"),
    }
    assert_eq!(
        session.dropped_frames(),
        14,
        "only the initial seek-forward"
    );
}
