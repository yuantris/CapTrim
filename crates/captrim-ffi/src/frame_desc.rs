//! Binary frame plan for renderers.
//!
//! [`crate::captrim_preview_step`] returns JSON, which is fine for script
//! front-ends and terrible for a compositor: it allocates a `String`, walks it
//! with a JSON writer, and hands back a pointer the caller has to remember to
//! free — at 60 Hz, on a deadline. [`crate::captrim_preview_step_frame`] is the
//! same decision written into caller-owned memory.
//!
//! Both paths share one implementation (`PreviewSession::step`) so a renderer
//! and a JS test can never disagree about which frame is due.

use crate::preview::{captrim_preview_t, preview_from, state};
use crate::{ffi_guard, Error};
use captrim_core::{effects_mask, LayerKind, Step};
use std::os::raw::c_int;

/// Must match `CAPTRIM_MAX_LAYERS` in captrim.h.
pub const MAX_LAYERS: usize = 8;

/// Must match `captrim_layer_desc_t`. `repr(C)` with only scalars, so the Rust
/// and C layouts are identical on every ABI this SDK targets.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct CaptrimLayerDesc {
    pub clip_id: u64,
    pub source_id: u64,
    pub source_us: i64,
    pub z: i32,
    pub kind: i32,
    pub scale: f32,
    pub dx: f32,
    pub dy: f32,
    pub rotation_deg: f32,
    pub opacity: f32,
    /// Bit mask of the v1 effects (CAPTRIM_EFFECT_* in captrim.h), packed from
    /// `Layer.effects` by the core so the renderer needs no vocabulary of its
    /// own. Kept last so every existing field keeps its offset.
    pub effects: u32,
}

/// Must match `captrim_frame_desc_t`.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct CaptrimFrameDesc {
    pub t_us: i64,
    pub frame_index: u64,
    pub width: u32,
    pub height: u32,
    pub layer_count: i32,
    pub layer_overflow: i32,
    pub audio_count: i32,
    pub layers: [CaptrimLayerDesc; MAX_LAYERS],
}

impl Default for CaptrimFrameDesc {
    fn default() -> Self {
        Self {
            t_us: 0,
            frame_index: 0,
            width: 0,
            height: 0,
            layer_count: 0,
            layer_overflow: 0,
            audio_count: 0,
            layers: [CaptrimLayerDesc::default(); MAX_LAYERS],
        }
    }
}

/// Must match `captrim_layer_kind_t`.
fn layer_kind_code(kind: LayerKind) -> i32 {
    match kind {
        LayerKind::Video => 0,
        LayerKind::Subtitle => 1,
        LayerKind::Overlay => 2,
    }
}

/// Copy at most MAX_LAYERS layers, and report the overflow instead of hiding it.
fn fill(out: &mut CaptrimFrameDesc, plan: &captrim_core::FramePlan) -> Result<(), Error> {
    out.t_us = plan.t_us;
    out.frame_index = plan.frame_index;
    out.width = plan.width;
    out.height = plan.height;
    out.audio_count = plan.audio.len().min(i32::MAX as usize) as i32;
    out.layers = [CaptrimLayerDesc::default(); MAX_LAYERS];
    out.layer_count = 0;
    out.layer_overflow = 0;

    let mut written = 0usize;
    for layer in &plan.layers {
        if written == MAX_LAYERS {
            break;
        }
        out.layers[written] = CaptrimLayerDesc {
            clip_id: layer.clip_id,
            source_id: layer.source_id,
            source_us: layer.source_us,
            z: layer.z,
            kind: layer_kind_code(layer.kind),
            scale: layer.transform.scale,
            dx: layer.transform.dx,
            dy: layer.transform.dy,
            rotation_deg: layer.transform.rotation_deg,
            opacity: layer.opacity,
            effects: effects_mask(&layer.effects),
        };
        written += 1;
    }
    out.layer_count = written as i32;
    out.layer_overflow = (plan.layers.len() - written).min(i32::MAX as usize) as i32;
    Ok(())
}

/// Step kinds, mirroring `captrim_step_kind_t`.
const STEP_IDLE: i32 = 0;
const STEP_PRESENT: i32 = 1;
const STEP_WAIT_FRAME: i32 = 2;
const STEP_END: i32 = 3;

#[no_mangle]
pub extern "C" fn captrim_preview_step_frame(
    preview: *mut captrim_preview_t,
    now_wall_us: i64,
    out_frame: *mut CaptrimFrameDesc,
    out_kind: *mut c_int,
    out_due_wall_us: *mut i64,
) -> i32 {
    ffi_guard(|| {
        if out_kind.is_null() {
            return Err(Error::invalid("out_kind is null"));
        }
        let p = preview_from(preview)?;
        let step = {
            let mut st = state(&p)?;
            p.sync_from_engine(&mut st)?;
            st.session.step(now_wall_us)
        };

        match step {
            Step::Present { frame } => {
                if !out_frame.is_null() {
                    // SAFETY: the caller passes a writable `captrim_frame_desc_t`,
                    // as required by captrim.h; we only write plain scalars.
                    let out = unsafe { &mut *out_frame };
                    fill(out, &frame)?;
                }
                unsafe { *out_kind = STEP_PRESENT };
            }
            Step::WaitFrame { due_wall_us } => {
                if !out_due_wall_us.is_null() {
                    unsafe { *out_due_wall_us = due_wall_us };
                }
                unsafe { *out_kind = STEP_WAIT_FRAME };
            }
            Step::Idle => unsafe { *out_kind = STEP_IDLE },
            Step::End { .. } => unsafe { *out_kind = STEP_END },
        }
        Ok(0)
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use captrim_core::{AudioMix, FramePlan, Layer, Transform};

    fn layer(z: i32, kind: LayerKind, source_us: i64) -> Layer {
        Layer {
            clip_id: 7,
            source_id: 3,
            uri: String::from("file:///a.mp4"),
            source_us,
            z,
            kind,
            transform: Transform {
                scale: 2.0,
                dx: 0.5,
                dy: -0.25,
                rotation_deg: 90.0,
            },
            opacity: 0.75,
            effects: Vec::new(),
        }
    }

    fn plan(layers: Vec<Layer>) -> FramePlan {
        FramePlan {
            t_us: 1_500_000,
            frame_index: 45,
            width: 1260,
            height: 2720,
            layers,
            audio: Vec::new(),
        }
    }

    #[test]
    fn copies_every_field_a_renderer_needs() {
        let mut out = CaptrimFrameDesc::default();
        fill(&mut out, &plan(vec![layer(0, LayerKind::Video, 16_667)])).unwrap();

        assert_eq!(out.t_us, 1_500_000);
        assert_eq!(out.frame_index, 45);
        assert_eq!(out.width, 1260);
        assert_eq!(out.height, 2720);
        assert_eq!(out.layer_count, 1);
        assert_eq!(out.layer_overflow, 0);

        let first = out.layers[0];
        assert_eq!(first.clip_id, 7);
        assert_eq!(first.source_id, 3);
        assert_eq!(first.source_us, 16_667);
        assert_eq!(first.kind, 0); // CAPTRIM_LAYER_VIDEO
        assert_eq!(first.z, 0);
        assert_eq!(first.scale, 2.0);
        assert_eq!(first.dx, 0.5);
        assert_eq!(first.dy, -0.25);
        assert_eq!(first.rotation_deg, 90.0);
        assert_eq!(first.opacity, 0.75);
    }

    #[test]
    fn layer_kinds_match_the_c_header() {
        assert_eq!(layer_kind_code(LayerKind::Video), 0);
        assert_eq!(layer_kind_code(LayerKind::Subtitle), 1);
        assert_eq!(layer_kind_code(LayerKind::Overlay), 2);
    }

    #[test]
    fn effects_are_packed_into_the_layer_bit_mask() {
        let mut l = layer(0, LayerKind::Video, 0);
        l.effects = vec!["blur".into(), "hflip".into(), "invert".into()];
        let mut out = CaptrimFrameDesc::default();
        fill(&mut out, &plan(vec![l])).unwrap();

        // Bits match CAPTRIM_EFFECT_* in captrim.h (hflip=1<<0, invert=1<<6,
        // blur=1<<7); insertion order does not matter.
        assert_eq!(out.layers[0].effects, (1 << 0) | (1 << 6) | (1 << 7));
    }

    #[test]
    fn effects_mask_is_zero_when_no_effects_are_set() {
        let mut out = CaptrimFrameDesc::default();
        fill(&mut out, &plan(vec![layer(0, LayerKind::Video, 0)])).unwrap();
        assert_eq!(out.layers[0].effects, 0);
    }

    #[test]
    fn overflow_is_reported_not_swallowed() {
        // MAX_LAYERS + 3 layers: the first MAX_LAYERS are described, the rest are
        // counted so a caller can tell a busy frame from a frame that lost data.
        let layers = (0..(MAX_LAYERS as i32 + 3))
            .map(|i| layer(i, LayerKind::Video, i as i64 * 1000))
            .collect();
        let mut out = CaptrimFrameDesc::default();
        fill(&mut out, &plan(layers)).unwrap();

        assert_eq!(out.layer_count as usize, MAX_LAYERS);
        assert_eq!(out.layer_overflow, 3);
        assert_eq!(
            out.layers[MAX_LAYERS - 1].source_us,
            (MAX_LAYERS as i64 - 1) * 1000
        );
    }

    #[test]
    fn refilling_a_reused_struct_leaves_no_stale_layers() {
        let mut out = CaptrimFrameDesc::default();
        fill(
            &mut out,
            &plan(vec![
                layer(0, LayerKind::Video, 1000),
                layer(1, LayerKind::Video, 2000),
            ]),
        )
        .unwrap();
        assert_eq!(out.layer_count, 2);

        fill(&mut out, &plan(Vec::new())).unwrap();
        assert_eq!(out.layer_count, 0);
        // Slot 1 must not still hold the previous frame's second layer.
        assert_eq!(out.layers[1].source_us, 0);
    }

    #[test]
    fn audio_is_counted_without_copying() {
        let mut audio_plan = plan(Vec::new());
        audio_plan.audio = vec![AudioMix {
            clip_id: 1,
            source_id: 2,
            uri: String::from("file:///a.mp4"),
            source_us: 0,
            volume: 1.0,
        }];
        let mut out = CaptrimFrameDesc::default();
        fill(&mut out, &audio_plan).unwrap();
        assert_eq!(out.audio_count, 1);
    }
}
