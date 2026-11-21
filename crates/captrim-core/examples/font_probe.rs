//! Which face of a font file actually rasterises CJK ink with fontdue?
//!
//! `cargo run -p captrim-core --example font_probe -- <font.ttf|ttc>`
//!
//! Prints, per collection index, the bitmap size and ink coverage for the
//! self-check caption. This is how the Android font trap was found: the
//! device's `NotoSansCJK-Regular.ttc` parses fine but every face renders
//! nothing (CFF outlines, which fontdue cannot rasterise), while MIUI's
//! `MiSansVF.ttf` produces ~3.5k ink pixels. [`captrim_core::text::has_truetype_outlines`]
//! encodes that check for the runtime font probe.

use captrim_core::text::{render_text, FontSpec, TextStyle};

fn main() {
    let path = std::env::args().nth(1).expect("font path");
    let text = "字幕烧录 · Overlay";
    for index in 0..8u32 {
        let spec = FontSpec::new(&path).with_collection_index(index);
        let style = TextStyle::subtitle(540, 960);
        match render_text(text, &spec, &style) {
            Ok(bitmap) => {
                let mut ink = 0usize;
                let mut opaque = 0usize;
                let mut i = 0usize;
                while i + 3 < bitmap.pixels.len() {
                    let alpha = bitmap.pixels[i + 3];
                    if alpha != 0 {
                        ink += 1;
                        if alpha == 255 {
                            opaque += 1;
                        }
                    }
                    i += 4;
                }
                println!(
                    "index {index}: {}x{} ink={ink} opaque={opaque}",
                    bitmap.width, bitmap.height
                );
            }
            Err(e) => println!("index {index}: error {e}"),
        }
    }
}
