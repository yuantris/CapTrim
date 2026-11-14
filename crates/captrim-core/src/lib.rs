//! CapTrim core: platform-independent editing model.
//!
//! No FFI, no OS threads, no platform SDK dependency. This crate must build
//! and test on the host, on Android, and on HarmonyOS from the same source.

pub mod error;
pub mod media;
pub mod model;
pub mod time;
pub mod timeline;

pub use error::{code_message, Error, Result};
pub use media::{MediaInfo, StreamInfo, StreamKind};
pub use model::{Clip, Id, MediaSource, Timeline, Track, TrackKind, Transform, DRAFT_VERSION};
pub use time::Rational;
pub use timeline::{Command, Editor};
