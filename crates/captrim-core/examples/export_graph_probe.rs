//! Host-side proof that a compiled export graph really produces the planned
//! picture. Two scenes, rendered with FFmpeg and checked against the plan:
//!
//! 1. three segments: a plain clip, a 2x-speed clip, and a transformed clip
//!    (half size, 30° clockwise, nudged right/up);
//! 2. a cross-dissolve with a picture-in-picture layer on top of it.
//!
//! Usage: `cargo run -p captrim-core --example export_graph_probe -- <ffmpeg-bin-dir> [work-dir]`
//!
//! The graph is the thing under test; the fixtures are synthetic so the probe
//! does not depend on any sample media.

use captrim_core::export::plan_export;
use captrim_core::model::{
    Clip, MediaSource, Timeline, Track, TrackKind, Transform, Transition, TransitionKind,
};
use captrim_core::time::Rational;
use captrim_core::{compile_export_graph, GraphSettings};
use std::path::{Path, PathBuf};
use std::process::Command;

fn run(exe: &Path, args: &[&str]) -> String {
    let out = Command::new(exe)
        .args(args)
        .output()
        .unwrap_or_else(|e| panic!("failed to run {}: {e}", exe.display()));
    if !out.status.success() {
        panic!(
            "{} {:?} failed:\n{}",
            exe.display(),
            args,
            String::from_utf8_lossy(&out.stderr)
        );
    }
    String::from_utf8_lossy(&out.stdout).into_owned()
}

fn run_stderr(exe: &Path, args: &[&str]) -> String {
    let out = Command::new(exe)
        .args(args)
        .output()
        .unwrap_or_else(|e| panic!("failed to run {}: {e}", exe.display()));
    assert!(
        out.status.success(),
        "{} {:?} failed:\n{}",
        exe.display(),
        args,
        String::from_utf8_lossy(&out.stderr)
    );
    String::from_utf8_lossy(&out.stderr).into_owned()
}

fn base_timeline(a: &Path, b: &Path) -> Timeline {
    let mut timeline = Timeline {
        width: 540,
        height: 960,
        frame_rate: Rational::new(30, 1).unwrap(),
        ..Timeline::default()
    };
    timeline.media.push(MediaSource {
        id: 1,
        uri: a.to_string_lossy().into_owned(),
        info: None,
    });
    timeline.media.push(MediaSource {
        id: 2,
        uri: b.to_string_lossy().into_owned(),
        info: None,
    });
    timeline
}

/// Render one timeline, then check it against its own plan.
fn render_scene(
    name: &str,
    timeline: &Timeline,
    ffmpeg: &Path,
    ffprobe: &Path,
    work: &Path,
    frames_to_grab: &[(&str, &str)],
) {
    let plan = plan_export(timeline);
    let graph = compile_export_graph(
        timeline,
        &plan,
        &GraphSettings {
            filter_suffix: "fps=30,format=yuv420p".to_string(),
            ..GraphSettings::default()
        },
    )
    .expect("compile");
    println!("\n=== {name} ===");
    println!("filter_complex:\n{}", graph.filter_complex);

    let dst = work.join(format!("{name}.mp4"));
    let dst_s = dst.to_string_lossy().into_owned();
    let mut argv: Vec<String> = vec!["-v".into(), "error".into()];
    argv.extend(graph.args(&dst_s));
    let argv_ref: Vec<&str> = argv.iter().map(|s| s.as_str()).collect();
    run(ffmpeg, &argv_ref);

    let planned_end_us = plan.segments.last().map(|s| s.end_us).unwrap_or(0);
    let planned_frames = plan.segments.last().map(|s| s.end_frame).unwrap_or(0);
    println!(
        "planned: {planned_frames} frames / {:.3}s in {} segments",
        planned_end_us as f64 / 1e6,
        plan.segments.len()
    );

    let probe = run(
        ffprobe,
        &[
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-count_frames",
            "-show_entries",
            "stream=width,height,nb_read_frames:format=duration",
            "-of",
            "default=nw=1",
            dst.to_str().unwrap(),
        ],
    );
    let mut width = 0u32;
    let mut height = 0u32;
    let mut frames = 0u64;
    let mut duration = 0f64;
    for line in probe.lines() {
        if let Some((k, v)) = line.split_once('=') {
            match k {
                "width" => width = v.trim().parse().unwrap_or(0),
                "height" => height = v.trim().parse().unwrap_or(0),
                "nb_read_frames" => frames = v.trim().parse().unwrap_or(0),
                "duration" => duration = v.trim().parse().unwrap_or(0.0),
                _ => {}
            }
        }
    }
    println!("probe: {width}x{height}, {frames} frames, {duration:.3}s");

    assert_eq!(
        (width, height),
        (timeline.width, timeline.height),
        "canvas must match the timeline"
    );
    assert_eq!(frames, planned_frames, "frame count must match the plan");
    assert!(
        (duration - planned_end_us as f64 / 1e6).abs() < 0.05,
        "duration {duration} must match the plan"
    );

    // Audio is part of the same render, so check the encoded track rather than
    // the plan: 48k stereo, and audible inside the window we care about.
    let planned_audio = plan.segments.iter().any(|s| !s.audio.is_empty());
    if planned_audio {
        let aprobe = run(
            ffprobe,
            &[
                "-v",
                "error",
                "-select_streams",
                "a:0",
                "-show_entries",
                "stream=sample_rate,channels",
                "-of",
                "default=nw=1",
                dst.to_str().unwrap(),
            ],
        );
        let (mut rate, mut channels) = (0u32, 0u32);
        for line in aprobe.lines() {
            if let Some((k, v)) = line.split_once('=') {
                match k {
                    "sample_rate" => rate = v.trim().parse().unwrap_or(0),
                    "channels" => channels = v.trim().parse().unwrap_or(0),
                    _ => {}
                }
            }
        }
        assert_eq!((rate, channels), (48_000, 2), "audio must be 48k stereo");
        let loud = run_stderr(
            ffmpeg,
            &[
                "-v",
                "info",
                "-i",
                dst.to_str().unwrap(),
                "-af",
                &format!(
                    "atrim=start={}:end={},volumedetect",
                    0.4 * planned_end_us as f64 / 1e6,
                    0.4 * planned_end_us as f64 / 1e6 + 0.5
                ),
                "-f",
                "null",
                "-",
            ],
        );
        let mean = loud
            .lines()
            .find_map(|line| line.split("mean_volume:").nth(1))
            .and_then(|rest| {
                rest.trim()
                    .trim_end_matches(" dB")
                    .trim()
                    .parse::<f64>()
                    .ok()
            })
            .unwrap_or(f64::NAN);
        println!("audio window mean_volume: {mean:.1} dB");
        assert!(
            mean.is_finite() && mean > -45.0,
            "the window must contain audible programme, not silence ({mean} dB)"
        );
    }

    for (t, suffix) in frames_to_grab {
        let out = work.join(format!("{name}-{suffix}.png"));
        run(
            ffmpeg,
            &[
                "-v",
                "error",
                "-ss",
                t,
                "-i",
                dst.to_str().unwrap(),
                "-frames:v",
                "1",
                "-y",
                out.to_str().unwrap(),
            ],
        );
        println!("frame at {t}s -> {}", out.display());
    }
}

fn window_mean_db(ffmpeg: &Path, file: &Path, start_s: f64, dur_s: f64) -> f64 {
    let filter = format!(
        "atrim=start={start_s:.3}:end={dur:.3},volumedetect",
        dur = start_s + dur_s
    );
    let log = run_stderr(
        ffmpeg,
        &[
            "-v",
            "info",
            "-i",
            file.to_str().unwrap(),
            "-af",
            &filter,
            "-f",
            "null",
            "-",
        ],
    );
    log.lines()
        .find_map(|line| line.split("mean_volume:").nth(1))
        .and_then(|rest| {
            rest.trim()
                .trim_end_matches(" dB")
                .trim()
                .parse::<f64>()
                .ok()
        })
        .unwrap_or(f64::NAN)
}

fn main() {
    let mut args = std::env::args().skip(1);
    let bin = PathBuf::from(
        args.next()
            .unwrap_or_else(|| "build/ffmpeg-host/ffmpeg-9.1-essentials_build/bin".to_string()),
    );
    let work = PathBuf::from(args.next().unwrap_or_else(|| "build".to_string()));
    std::fs::create_dir_all(&work).expect("work dir");
    let ffmpeg = bin.join(if cfg!(windows) {
        "ffmpeg.exe"
    } else {
        "ffmpeg"
    });
    let ffprobe = bin.join(if cfg!(windows) {
        "ffprobe.exe"
    } else {
        "ffprobe"
    });

    // Two distinct sources so a wrong trim shows up as the wrong picture, each
    // with its own tone so a wrong audio range or a lost channel is audible.
    let a = work.join("graph-a.mp4");
    let b = work.join("graph-b.mp4");
    for (dst, pattern, tone) in [
        (&a, "testsrc2", "sine=frequency=440:sample_rate=48000"),
        (&b, "smptebars", "sine=frequency=880:sample_rate=48000"),
    ] {
        let src = format!("{pattern}=size=640x360:rate=30:duration=4");
        run(
            &ffmpeg,
            &[
                "-v",
                "error",
                "-f",
                "lavfi",
                "-i",
                &src,
                "-f",
                "lavfi",
                "-i",
                tone,
                "-c:v",
                "libx264",
                "-pix_fmt",
                "yuv420p",
                "-c:a",
                "aac",
                "-shortest",
                "-y",
                dst.to_str().unwrap(),
            ],
        );
    }

    // Scene 1: plain / speed / transformed.
    let mut scene1 = base_timeline(&a, &b);
    let mut fast = Clip::new(2, 1_000_000, 3_000_000, 2_000_000);
    fast.id = 4;
    fast.speed = Rational::new(2, 1).unwrap();
    let mut first = Clip::new(1, 0, 2_000_000, 0);
    first.id = 3;
    let mut spun = Clip::new(1, 0, 2_000_000, 3_000_000);
    spun.id = 5;
    spun.transform = Transform {
        scale: 0.5,
        dx: 0.15,
        dy: -0.10,
        rotation_deg: 30.0,
    };
    scene1.tracks.push(Track {
        id: 2,
        kind: TrackKind::MainVideo,
        clips: vec![first, fast, spun],
    });
    render_scene(
        "scene1",
        &scene1,
        &ffmpeg,
        &ffprobe,
        &work,
        &[("0.5", "a"), ("2.5", "b"), ("4.0", "c")],
    );

    // Scene 2: cross-dissolve (0.5s window at 2.0s) with a PiP on top.
    let mut scene2 = base_timeline(&a, &b);
    let mut outgoing = Clip::new(1, 0, 2_000_000, 0);
    outgoing.id = 3;
    let mut incoming = Clip::new(2, 0, 2_000_000, 2_000_000);
    incoming.id = 4;
    incoming.transition_in = Some(Transition {
        kind: TransitionKind::Fade,
        duration_us: 500_000,
    });
    scene2.tracks.push(Track {
        id: 2,
        kind: TrackKind::MainVideo,
        clips: vec![outgoing, incoming],
    });
    let mut pip = Clip::new(2, 0, 4_000_000, 0);
    pip.id = 6;
    // Silent: the PiP stands in for a sticker-like insert. Its own audio would
    // sum with the main track and hide the crossfade we are measuring.
    pip.volume = 0.0;
    pip.transform = Transform {
        scale: 0.25,
        dx: 0.30,
        dy: -0.30,
        rotation_deg: 0.0,
    };
    scene2.tracks.push(Track {
        id: 5,
        kind: TrackKind::Overlay,
        clips: vec![pip],
    });
    render_scene(
        "scene2",
        &scene2,
        &ffmpeg,
        &ffprobe,
        &work,
        &[("0.5", "before"), ("2.2", "mid"), ("3.0", "after")],
    );

    println!("\nOK");

    // The dissolve window is 2.0 -> 2.5s. Two uncorrelated tones at
    // complementary gains sum to sqrt((1-u)^2 + u^2), which dips by ~2 dB in
    // the middle of the window. A hard cut (one source only) would stay flat,
    // so this is the real proof that both clips sound through the seam.
    let scene2_file = work.join("scene2.mp4");
    let edge = window_mean_db(&ffmpeg, &scene2_file, 2.03, 0.08).min(window_mean_db(
        &ffmpeg,
        &scene2_file,
        2.42,
        0.08,
    ));
    let mid = window_mean_db(&ffmpeg, &scene2_file, 2.22, 0.06);
    println!("crossfade window: edges {edge:.1} dB, middle {mid:.1} dB");
    assert!(
        mid.is_finite() && edge.is_finite() && mid < edge - 1.0,
        "the window must mix both sources (edges {edge}, middle {mid})"
    );
}
