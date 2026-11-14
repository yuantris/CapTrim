//! End-to-end P0 flow on the pure model: import -> probe info -> edit ->
//! split/trim -> draft save/restore -> timeline queries used by the renderer.

use captrim_core::{Clip, Command, Editor, MediaInfo, Rational, StreamInfo, StreamKind, TrackKind};

fn attach_probe(e: &mut Editor, media_id: u64) {
    // Emulates what the native FFmpeg backend writes back after a probe.
    let mut draft = e.timeline().clone();
    let info = MediaInfo {
        uri: "file:///clip.mp4".into(),
        format: "mov,mp4,m4a,3gp,3g2,mj2".into(),
        duration_us: 12_000_000,
        bitrate_bps: 5_000_000,
        streams: vec![
            StreamInfo {
                index: 0,
                kind: StreamKind::Video,
                codec: "h264".into(),
                time_base: Rational::new(1, 30_000).unwrap(),
                duration_us: 12_000_000,
                width: Some(1920),
                height: Some(1080),
                frame_rate: Some(Rational::new(30, 1).unwrap()),
                bitrate_bps: 4_800_000,
                ..Default::default()
            },
            StreamInfo {
                index: 1,
                kind: StreamKind::Audio,
                codec: "aac".into(),
                time_base: Rational::new(1, 48_000).unwrap(),
                duration_us: 12_000_000,
                sample_rate: Some(48_000),
                channels: Some(2),
                bitrate_bps: 192_000,
                ..Default::default()
            },
        ],
    };
    draft
        .media
        .iter_mut()
        .find(|m| m.id == media_id)
        .unwrap()
        .info = Some(info.clone());
    assert!(info.supports_stream_copy_mp4());
    *e = Editor::from_timeline(draft).unwrap();
}

#[test]
fn p0_flow() {
    let mut e = Editor::new();

    let media_id = e
        .apply(&Command::AddMedia {
            uri: "file:///clip.mp4".into(),
        })
        .unwrap()
        .unwrap();
    attach_probe(&mut e, media_id);

    let track_id = e
        .apply(&Command::AddTrack {
            kind: TrackKind::MainVideo,
        })
        .unwrap()
        .unwrap();
    let clip_id = e
        .apply(&Command::InsertClip {
            track_id,
            clip: Clip::new(media_id, 0, 10_000_000, 0),
        })
        .unwrap()
        .unwrap();

    // Split, trim the right half, then speed it up 2x.
    let right_id = e
        .apply(&Command::SplitClip {
            clip_id,
            at_us: 4_000_000,
        })
        .unwrap()
        .unwrap();
    e.apply(&Command::SetOutPoint {
        clip_id: right_id,
        out_us: 8_000_000,
    })
    .unwrap();
    e.apply(&Command::SetSpeed {
        clip_id: right_id,
        speed: Rational::new(2, 1).unwrap(),
    })
    .unwrap();
    e.apply(&Command::SetVolume {
        clip_id: right_id,
        volume: 0.5,
    })
    .unwrap();

    // Left: 0..4s. Right: source 4..8s at 2x = 2s, so total timeline is 6s.
    assert_eq!(e.timeline().duration_us(), 6_000_000);
    assert_eq!(e.timeline().clips_at(1_000_000).len(), 1);
    assert_eq!(e.timeline().clips_at(5_000_000).len(), 1);
    assert!(e.timeline().clips_at(7_000_000).is_empty());

    // Draft persistence + undo/redo across a restore boundary.
    let draft = e.draft_json().unwrap();
    let mut restored = Editor::from_draft(&draft).unwrap();
    assert_eq!(restored.timeline(), e.timeline());
    assert!(!restored.can_undo());
    assert!(restored
        .apply_json(r#"{"type":"set_out_point","clip_id":1,"out_us":9000000}"#)
        .is_err());
    assert!(restored
        .apply_json(r#"{"type":"set_out_point","clip_id":3,"out_us":9000000}"#)
        .is_ok());
    assert!(restored.undo());
    assert_eq!(restored.timeline(), e.timeline());
}
