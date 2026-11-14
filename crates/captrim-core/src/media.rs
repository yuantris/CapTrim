//! Probe results. This is the JSON contract between the FFmpeg backend and
//! every platform front-end (Kotlin / ArkTS / Rust tests).

use crate::time::Rational;
use serde::{Deserialize, Serialize};

#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq, Default)]
#[serde(rename_all = "snake_case")]
pub enum StreamKind {
    Video,
    Audio,
    Subtitle,
    Data,
    #[default]
    Unknown,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct StreamInfo {
    pub index: i32,
    pub kind: StreamKind,
    /// FFmpeg codec name, e.g. `h264`, `hevc`, `aac`.
    pub codec: String,
    pub time_base: Rational,
    pub duration_us: i64,
    pub width: Option<u32>,
    pub height: Option<u32>,
    pub frame_rate: Option<Rational>,
    pub sample_rate: Option<u32>,
    pub channels: Option<u32>,
    pub rotation_deg: i32,
    pub bitrate_bps: i64,
}

impl Default for StreamInfo {
    fn default() -> Self {
        Self {
            index: 0,
            kind: StreamKind::Unknown,
            codec: String::new(),
            time_base: Rational::ONE,
            duration_us: 0,
            width: None,
            height: None,
            frame_rate: None,
            sample_rate: None,
            channels: None,
            rotation_deg: 0,
            bitrate_bps: 0,
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Default)]
pub struct MediaInfo {
    pub uri: String,
    pub format: String,
    pub duration_us: i64,
    pub bitrate_bps: i64,
    pub streams: Vec<StreamInfo>,
}

impl MediaInfo {
    pub fn video_streams(&self) -> impl Iterator<Item = &StreamInfo> {
        self.streams.iter().filter(|s| s.kind == StreamKind::Video)
    }

    pub fn audio_streams(&self) -> impl Iterator<Item = &StreamInfo> {
        self.streams.iter().filter(|s| s.kind == StreamKind::Audio)
    }

    pub fn primary_video(&self) -> Option<&StreamInfo> {
        self.video_streams().next()
    }

    pub fn primary_audio(&self) -> Option<&StreamInfo> {
        self.audio_streams().next()
    }

    /// True when a container/stream combination can be exported via stream
    /// copy (P0 remux path) instead of a full transcode.
    pub fn supports_stream_copy_mp4(&self) -> bool {
        let video_ok = self
            .video_streams()
            .all(|s| matches!(s.codec.as_str(), "h264" | "hevc"));
        let audio_ok = self
            .audio_streams()
            .all(|s| matches!(s.codec.as_str(), "aac" | "mp3"));
        video_ok && audio_ok && self.primary_video().is_some()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn video(codec: &str) -> StreamInfo {
        StreamInfo {
            index: 0,
            kind: StreamKind::Video,
            codec: codec.into(),
            width: Some(1920),
            height: Some(1080),
            ..Default::default()
        }
    }

    #[test]
    fn stream_copy_is_rejected_for_unsupported_codecs() {
        let mut info = MediaInfo {
            format: "mov,mp4,m4a,3gp,3g2,mj2".into(),
            ..Default::default()
        };
        info.streams.push(video("h264"));
        assert!(info.supports_stream_copy_mp4());

        info.streams.push(video("mpeg4"));
        assert!(!info.supports_stream_copy_mp4());
    }

    #[test]
    fn json_contract_round_trip() {
        let mut info = MediaInfo {
            uri: "file:///tmp/a.mp4".into(),
            format: "mov,mp4,m4a,3gp,3g2,mj2".into(),
            duration_us: 10_000_000,
            bitrate_bps: 4_000_000,
            ..Default::default()
        };
        info.streams.push(StreamInfo {
            frame_rate: Some(Rational::new(30_000, 1001).unwrap()),
            ..video("h264")
        });
        let json = serde_json::to_string(&info).unwrap();
        let back: MediaInfo = serde_json::from_str(&json).unwrap();
        assert_eq!(info, back);
        assert_eq!(back.primary_video().unwrap().width, Some(1920));
    }
}
