//! Dump a rasterised subtitle block to a PNG for eyeballing.
//!
//! `cargo run -p captrim-core --example text_preview -- [font] [out.png]`
//!
//! Composites the bitmap over a checkerboard so both the ink and the outline
//! are visible; useful when tuning font metrics without a device round-trip.

use captrim_core::text::{render_text, resolve_default_font, FontSpec, TextStyle};

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let spec = match args.first() {
        Some(path) => FontSpec::new(path),
        None => match resolve_default_font() {
            Ok(spec) => spec,
            Err(err) => {
                eprintln!("no font found: {err}");
                std::process::exit(1);
            }
        },
    };
    let out = args
        .get(1)
        .cloned()
        .unwrap_or_else(|| "build/text-preview.png".to_string());

    let canvas = (1080u32, 1920u32);
    let style = TextStyle::subtitle(canvas.0, canvas.1);
    let samples = [
        "CapTrim 字幕预览 v1",
        "这一句用来验证换行：字幕很长的时候应该自动折成两行，并且保持居中对齐。",
        "Line one\nLine two",
    ];

    let mut blocks = Vec::new();
    for text in samples {
        let bitmap = render_text(text, &spec, &style).expect("rasterise");
        eprintln!(
            "{:?} -> {}x{}",
            text.chars().take(12).collect::<String>(),
            bitmap.width,
            bitmap.height
        );
        blocks.push(bitmap);
    }

    let width = blocks.iter().map(|b| b.width).max().unwrap_or(1);
    let height: u32 = blocks.iter().map(|b| b.height).sum::<u32>() + 3 * 24;
    let mut pixels = vec![0u8; width as usize * height as usize * 4];

    // Mid-grey background with a checkerboard, so white ink and the black
    // outline are both visible.
    for y in 0..height {
        for x in 0..width {
            let checker = ((x / 24) + (y / 24)) % 2 == 0;
            let value: u8 = if checker { 96 } else { 140 };
            let base = (y as usize * width as usize + x as usize) * 4;
            pixels[base] = value;
            pixels[base + 1] = value;
            pixels[base + 2] = value;
            pixels[base + 3] = 255;
        }
    }

    let mut y_offset = 12u32;
    for block in &blocks {
        let x_offset = (width - block.width) / 2;
        for y in 0..block.height {
            for x in 0..block.width {
                let src = ((y * block.width + x) as usize) * 4;
                let a = block.pixels[src + 3] as f32 / 255.0;
                if a <= 0.0 {
                    continue;
                }
                let dst = ((y + y_offset) as usize * width as usize + (x + x_offset) as usize) * 4;
                for c in 0..3 {
                    let under = pixels[dst + c] as f32;
                    let over = block.pixels[src + c] as f32;
                    pixels[dst + c] = (over * a + under * (1.0 - a)).round() as u8;
                }
                pixels[dst + 3] = 255;
            }
        }
        y_offset += block.height + 24;
    }

    if let Some(parent) = std::path::Path::new(&out).parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    let file = std::fs::File::create(&out).expect("create png");
    let mut encoder = png::Encoder::new(file, width, height);
    encoder.set_color(png::ColorType::Rgba);
    encoder.set_depth(png::BitDepth::Eight);
    let mut writer = encoder.write_header().expect("png header");
    writer.write_image_data(&pixels).expect("png data");
    eprintln!("wrote {out} ({width}x{height})");
}
