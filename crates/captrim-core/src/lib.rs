//! CapTrim core: platform-independent editing model.
//!
//! No FFI, no OS threads, no platform SDK dependency. This crate must build
//! and test on the host, on Android, and on HarmonyOS from the same source.

pub mod clock;
pub mod error;
pub mod export;
pub mod media;
pub mod model;
pub mod preview;
pub mod render;
pub mod time;
pub mod timeline;

pub use clock::PlaybackClock;
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
pub use time::Rational;
pub use timeline::{Command, Editor};
