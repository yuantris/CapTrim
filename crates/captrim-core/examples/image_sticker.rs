//! Draw the demo sticker shipped as `rawfile/sticker.png`.
//!
//! The point of the asset is the alpha channel: a ring on a transparent
//! background makes a wrong answer obvious on screen (a decoder that drops alpha
//! paints a black square around it), and its circular edge shows whether the
//! resampler keeps edges smooth.
//!
//! Regenerate with:
//! `cargo run -p captrim-core --example image_sticker -- \
//!    platforms/ohos/entry/src/main/resources/rawfile/sticker.png`

use image::{Rgba, RgbaImage};
use std::path::PathBuf;

const SIZE: u32 = 256;

fn main() {
    let path = std::env::args()
        .nth(1)
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("sticker.png"));

    let mut image = RgbaImage::from_pixel(SIZE, SIZE, Rgba([0, 0, 0, 0]));
    let centre = SIZE as f32 / 2.0;
    let (outer, inner) = (118.0f32, 64.0f32);
    for y in 0..SIZE {
        for x in 0..SIZE {
            let dx = x as f32 + 0.5 - centre;
            let dy = y as f32 + 0.5 - centre;
            let radius = (dx * dx + dy * dy).sqrt();
            // 1px anti-aliased edges: a hard binary edge would alias badly at
            // the 0.3-scale the demo draws it at.
            let coverage =
                (outer - radius + 0.5).clamp(0.0, 1.0) - (inner - radius + 0.5).clamp(0.0, 1.0);
            if coverage <= 0.0 {
                continue;
            }
            let alpha = (coverage * 255.0).round() as u8;
            image.put_pixel(x, y, Rgba([255, 122, 69, alpha]));
        }
    }

    image
        .save_with_format(&path, image::ImageFormat::Png)
        .unwrap_or_else(|err| panic!("cannot write {}: {err}", path.display()));
    println!("wrote {} ({}x{})", path.display(), SIZE, SIZE);
}
