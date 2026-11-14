use std::fmt;

/// Error taxonomy shared by the Rust core and the C ABI.
///
/// `code()` values are part of the public ABI: never reorder or reuse them.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Error {
    InvalidArgument(String),
    NotFound(String),
    OutOfRange(String),
    /// A media operation was requested but no FFmpeg backend was linked in.
    BackendUnavailable(String),
    Io(String),
    Decode(String),
    Encode(String),
    Cancelled,
    Internal(String),
}

impl Error {
    pub fn code(&self) -> i32 {
        match self {
            Error::InvalidArgument(_) => -1,
            Error::NotFound(_) => -2,
            Error::OutOfRange(_) => -3,
            Error::BackendUnavailable(_) => -4,
            Error::Io(_) => -5,
            Error::Decode(_) => -6,
            Error::Encode(_) => -7,
            Error::Cancelled => -8,
            Error::Internal(_) => -9,
        }
    }

    pub const PANIC_CODE: i32 = -10;

    pub fn invalid(msg: impl Into<String>) -> Self {
        Error::InvalidArgument(msg.into())
    }

    pub fn not_found(msg: impl Into<String>) -> Self {
        Error::NotFound(msg.into())
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::InvalidArgument(m) => write!(f, "invalid argument: {m}"),
            Error::NotFound(m) => write!(f, "not found: {m}"),
            Error::OutOfRange(m) => write!(f, "out of range: {m}"),
            Error::BackendUnavailable(m) => write!(f, "backend unavailable: {m}"),
            Error::Io(m) => write!(f, "io error: {m}"),
            Error::Decode(m) => write!(f, "decode error: {m}"),
            Error::Encode(m) => write!(f, "encode error: {m}"),
            Error::Cancelled => write!(f, "cancelled"),
            Error::Internal(m) => write!(f, "internal error: {m}"),
        }
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;

/// Human-readable text for a numeric C ABI status code.
pub fn code_message(code: i32) -> &'static str {
    match code {
        0 => "ok",
        -1 => "invalid argument",
        -2 => "not found",
        -3 => "out of range",
        -4 => "backend unavailable",
        -5 => "io error",
        -6 => "decode error",
        -7 => "encode error",
        -8 => "cancelled",
        -9 => "internal error",
        -10 => "panic caught at FFI boundary",
        _ => "unknown error",
    }
}
