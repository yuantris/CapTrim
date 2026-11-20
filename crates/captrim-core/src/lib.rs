//! CapTrim core: platform-independent editing model.
//!
//! No FFI, no OS threads, no platform SDK dependency. This crate must build
//! and test on the host, on Android, and on HarmonyOS from the same source.

pub mod clock;
pub mod effects;
pub mod error;
pub mod export;
pub mod media;
pub mod model;
pub mod preview;
pub mod render;
pub mod text;
pub mod time;
pub mod timeline;

pub use clock::PlaybackClock;
pub use effects::{
    effects_mask, effects_to_filter_desc, is_known_effect, CANONICAL_ORDER, EFFECT_BLUR,
    EFFECT_BRIGHTNESS, EFFECT_CONTRAST, EFFECT_GRAYSCALE, EFFECT_HFLIP, EFFECT_INVERT,
    EFFECT_SATURATION, EFFECT_VFLIP,
};
pub use error::{code_message, Error, Result};
pub use export::{
    frame_index_ceil, plan_export, ExportAudioItem, ExportPlan, ExportSegment, ExportVideoItem,
};
pub use media::{MediaInfo, StreamInfo, StreamKind};
pub use model::{Clip, Id, MediaSource, Timeline, Track, TrackKind, Transform, DRAFT_VERSION};
pub use preview::{PreviewSession, Step};
pub use render::{
    plan_audio_at, plan_frame, plan_frame_at_index, AudioMix, FramePlan, Layer, LayerKind,
};
pub use text::{
    default_font_specs, load_font, render_text, resolve_default_font, text_revision, FontSpec,
    TextAlign, TextBitmap, TextStyle,
};
pub use time::Rational;
pub use timeline::{Command, Editor, SUBTITLE_DEFAULT_DY};
