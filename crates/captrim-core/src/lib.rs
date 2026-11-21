//! CapTrim core: platform-independent editing model.
//!
//! No FFI, no OS threads, no platform SDK dependency. This crate must build
//! and test on the host, on Android, and on HarmonyOS from the same source.

pub mod clock;
pub mod effects;
pub mod encoders;
pub mod error;
pub mod export;
pub mod export_graph;
pub mod images;
pub mod media;
pub mod model;
pub mod preview;
pub mod render;
pub mod subtitles;
pub mod text;
pub mod time;
pub mod timeline;

pub use clock::PlaybackClock;
pub use effects::{
    available_effects, effects_mask, effects_to_filter_desc, effects_to_filter_desc_with,
    ffmpeg_filter, is_known_effect, CANONICAL_ORDER, EFFECT_BLUR, EFFECT_BRIGHTNESS,
    EFFECT_CONTRAST, EFFECT_GRAYSCALE, EFFECT_HFLIP, EFFECT_INVERT, EFFECT_SATURATION,
    EFFECT_VFLIP,
};
pub use error::{code_message, Error, Result};
pub use export::{
    frame_index_ceil, plan_export, ExportAudioItem, ExportPlan, ExportSegment, ExportVideoItem,
};
pub use export_graph::{compile as compile_export_graph, ExportGraph, GraphSettings};
pub use images::{
    fitted_size, image_revision, load_image, local_path, render_image, source_dimensions,
    ImageBitmap, SourceImage,
};
pub use media::{MediaInfo, StreamInfo, StreamKind};
pub use model::{
    Clip, Id, ImageAsset, MediaSource, Timeline, Track, TrackKind, Transform, DRAFT_VERSION,
};
pub use preview::{PreviewSession, Step};
pub use render::{
    plan_audio_at, plan_frame, plan_frame_at_index, AudioMix, FramePlan, Layer, LayerKind,
};
pub use subtitles::{burn_filter, plan_burn, BurnLayer};
pub use text::{
    default_font_specs, load_font, render_text, resolve_default_font, text_revision, FontSpec,
    TextAlign, TextBitmap, TextStyle,
};
pub use time::Rational;
pub use timeline::{Command, Editor, SUBTITLE_DEFAULT_DY};
