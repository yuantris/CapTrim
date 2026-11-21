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

    // Two distinct sources so a wrong trim shows up as the wrong picture.
    let a = work.join("graph-a.mp4");
    let b = work.join("graph-b.mp4");
    for (dst, pattern) in [(&a, "testsrc2"), (&b, "smptebars")] {
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
                "-c:v",
                "libx264",
                "-pix_fmt",
                "yuv420p",
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
}
