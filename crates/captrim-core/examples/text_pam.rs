//! Write one subtitle as a PAM (NetPBM P7, RGB_ALPHA) — the exact bytes the
//! export path hands to FFmpeg's `movie` filter.
//!
//! Used to debug the burn-in graph against a host FFmpeg without a device:
//!
//! ```text
//! cargo run -p captrim-core --example text_pam -- build/sub.pam 540 960 "字幕烧录"
//! ffmpeg -f lavfi -i testsrc2=size=540x960:rate=15:duration=2 -pix_fmt yuv420p \
//!   -filter_complex "[0:v]trim=duration=2,setpts=PTS-STARTPTS[bg];\
//!     movie=build/sub.pam[s];[s]loop=loop=-1:size=1:start=0[i];\
//!     [bg][i]overlay=x=0:y=800:enable='between(t,0.4,1.6)'[out]" -map "[out]" burn.mp4
//! ```

use captrim_core::text::{default_font_specs, load_font, render_text, TextStyle};
use std::io::Write;

fn main() {
    let mut args = std::env::args().skip(1);
    let path = args.next().unwrap_or_else(|| "build/sub.pam".into());
    let width: u32 = args
        .next()
        .and_then(|value| value.parse().ok())
        .unwrap_or(540);
    let height: u32 = args
        .next()
        .and_then(|value| value.parse().ok())
        .unwrap_or(960);
    let text = args
        .next()
        .unwrap_or_else(|| "字幕烧录 · Overlay".to_string());

    let font = default_font_specs()
        .into_iter()
        .find(|spec| load_font(spec).is_ok())
        .expect("no system font found");
    let bitmap =
        render_text(&text, &font, &TextStyle::subtitle(width, height)).expect("rasterise failed");

    let header = format!(
        "P7\nWIDTH {}\nHEIGHT {}\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n",
        bitmap.width, bitmap.height
    );
    let mut file = std::fs::File::create(&path).expect("create pam");
    file.write_all(header.as_bytes()).unwrap();
    file.write_all(&bitmap.pixels).unwrap();
    println!(
        "{path}: {}x{} (canvas {width}x{height})",
        bitmap.width, bitmap.height
    );
}
