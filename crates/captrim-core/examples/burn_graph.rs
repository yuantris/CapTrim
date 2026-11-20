//! Emit a caption + sticker burn graph for a host FFmpeg to execute.
//!
//! The device self-check proves the PAM files carry ink, but the burned MP4
//! cannot be pulled back (`hdc` is a shell user and the app sandbox is
//! unreadable). So this example runs the *same* `plan_burn` / `burn_filter`
//! code on the host, where the resulting picture can be inspected frame by
//! frame with the release FFmpeg build.
//!
//! `cargo run -p captrim-core --example burn_graph -- build/burn-demo`
//! writes `<dir>/sub_*.pam`, `<dir>/img_*.pam` and `<dir>/graph.txt` holding a
//! ready-to-use `-filter_script:v` file.

use captrim_core::images;
use captrim_core::model::{Clip, ImageAsset, Timeline, Track, TrackKind, Transform};
use captrim_core::subtitles::{burn_filter, plan_burn};
use captrim_core::text::default_font_specs;
use std::path::PathBuf;

fn main() {
    let dir = std::env::args()
        .nth(1)
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("build/burn-demo"));
    let sticker = std::env::args().nth(2).unwrap_or_else(|| {
        "platforms/ohos/entry/src/main/resources/rawfile/sticker.png".to_string()
    });
    std::fs::create_dir_all(&dir).expect("work dir");

    let mut timeline = Timeline {
        width: 540,
        height: 960,
        ..Timeline::default()
    };

    let mut caption = Clip::new(0, 0, 1_200_000, 400_000);
    caption.id = 1;
    caption.text = Some("字幕烧录 · Overlay".into());
    caption.transform.dy = captrim_core::timeline::SUBTITLE_DEFAULT_DY;
    timeline.tracks.push(Track {
        id: 1,
        kind: TrackKind::Subtitle,
        clips: vec![caption],
    });

    let (width, height) = images::source_dimensions(&sticker).expect("sticker size");
    let mut still = Clip::new(0, 0, 1_200_000, 400_000);
    still.id = 2;
    still.image = Some(ImageAsset::new(sticker.clone(), width, height));
    // Same placement the device self-check uses: top-right, quarter size.
    still.transform = Transform {
        scale: 0.25,
        dx: 0.3,
        dy: -0.35,
        rotation_deg: 0.0,
    };
    timeline.tracks.push(Track {
        id: 2,
        kind: TrackKind::Image,
        clips: vec![still],
    });

    let spec = default_font_specs()
        .into_iter()
        .find(|s| std::path::Path::new(&s.path).exists())
        .expect("a system font to rasterise the caption");

    let layers = plan_burn(&timeline, &dir, &spec).expect("plan burn");
    for layer in &layers {
        println!(
            "{} -> {}x{} at ({}, {})",
            layer.path, layer.width, layer.height, layer.x, layer.y
        );
    }
    // The device passes a real prefix (`trim=...,scale=...`) and suffix
    // (`fps=15`). On the host the graph is handed to `-filter_complex` instead,
    // where an empty prefix would leave `[in][bg0]` -- two labels and no
    // filter, which libavfilter rejects ("No such filter: ''"). `null` keeps the
    // shape honest without changing the pixels.
    let graph = burn_filter(&layers, "null", "").expect("filter graph");
    let path = dir.join("graph.txt");
    std::fs::write(&path, &graph).expect("write graph");
    println!("graph: {}", path.display());
    println!("{graph}");
}
