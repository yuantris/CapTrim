//! Host-side proof that a compiled export graph really produces the planned
//! picture: builds a 3-second, 2-segment timeline (one of them at 2x speed),
//! renders it with FFmpeg, then checks the file against the plan.
//!
//! Usage: `cargo run -p captrim-core --example export_graph_probe -- <ffmpeg-bin-dir> [work-dir]`
//!
//! The graph is the thing under test; the fixtures are synthetic so the probe
//! does not depend on any sample media.

use captrim_core::export::plan_export;
use captrim_core::model::{Clip, MediaSource, Timeline, Track, TrackKind, Transform};
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
    let mut fast = Clip::new(2, 1_000_000, 3_000_000, 2_000_000);
    fast.id = 4;
    fast.speed = Rational::new(2, 1).unwrap();
    let mut first = Clip::new(1, 0, 2_000_000, 0);
    first.id = 3;
    // Third segment exercises the geometry path: half size, turned 30 degrees
    // clockwise and nudged right/up, all in canvas units.
    let mut spun = Clip::new(1, 0, 2_000_000, 3_000_000);
    spun.id = 5;
    spun.transform = Transform {
        scale: 0.5,
        dx: 0.15,
        dy: -0.10,
        rotation_deg: 30.0,
    };
    timeline.tracks.push(Track {
        id: 2,
        kind: TrackKind::MainVideo,
        clips: vec![first, fast, spun],
    });

    let plan = plan_export(&timeline);
    let graph = compile_export_graph(
        &timeline,
        &plan,
        &GraphSettings {
            filter_suffix: "fps=30,format=yuv420p".to_string(),
            ..GraphSettings::default()
        },
    )
    .expect("compile");
    println!("inputs: {:?}", graph.inputs);
    println!("filter_complex:\n{}", graph.filter_complex);

    let dst = work.join("graph-out.mp4");
    let dst_s = dst.to_string_lossy().into_owned();
    let mut argv: Vec<String> = vec!["-v".into(), "error".into()];
    argv.extend(graph.args(&dst_s));
    let argv_ref: Vec<&str> = argv.iter().map(|s| s.as_str()).collect();
    run(&ffmpeg, &argv_ref);

    // The plan's last segment ends this far into the output.
    let planned_end_us = plan.segments.last().map(|s| s.end_us).unwrap_or(0);
    let planned_frames = plan.segments.last().map(|s| s.end_frame).unwrap_or(0);
    println!(
        "planned: {} frames / {:.3}s",
        planned_frames,
        planned_end_us as f64 / 1e6
    );

    let probe = run(
        &ffprobe,
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
    println!("probe:\n{probe}");

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

    assert_eq!(
        (width, height),
        (540, 960),
        "canvas must match the timeline"
    );
    assert_eq!(frames, planned_frames, "frame count must match the plan");
    assert!(
        (duration - planned_end_us as f64 / 1e6).abs() < 0.05,
        "duration {duration} must match the plan"
    );

    // Frames from each segment, for the eye: segment 0 is testsrc2 at 0.5s,
    // segment 1 is smptebars at 2.5s (its 2x source window), segment 2 is the
    // transformed testsrc2 at 4.0s.
    for (t, name) in [
        ("0.5", "graph-frame-a.png"),
        ("2.5", "graph-frame-b.png"),
        ("4.0", "graph-frame-c.png"),
    ] {
        let out = work.join(name);
        run(
            &ffmpeg,
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

    println!(
        "OK: {} frames, {:.3}s, {}x{}",
        frames, duration, width, height
    );
}
