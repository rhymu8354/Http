/// This is the enumeration of all the different kinds of errors which this
/// crate generates.
#[derive(Debug, Clone, thiserror::Error, PartialEq)]
pub enum Error {
    /// An error occurred with the message headers.
    #[error("Error in headers")]
    Headers(#[from] rhymessage::Error),

    /// The `Content-Length` header value is not valid.
    #[error("invalid Content-Length header value")]
    InvalidContentLength(std::num::ParseIntError),

    /// The message is too large to fit within the configured size limit.
    #[error("message exceeds maximum size limit")]
    MessageTooLong,

    /// The attached bytes did not parse as a valid HTTP request line.
    #[error("request line is invalid")]
    RequestLineInvalid(Vec<u8>),

    /// The method could not be parsed from the HTTP request line bytes
    /// attached.  Either there is extra whitespace, or the method is
    /// an empty string.
    #[error("unable to parse method from request line")]
    RequestLineNoMethodOrExtraWhitespace(Vec<u8>),

    /// The target URI could not be parsed from the HTTP request line bytes
    /// attached.  Either there is extra whitespace, or the target URI is
    /// an empty string.
    #[error("unable to parse method from request line")]
    RequestLineNoTargetOrExtraWhitespace(Vec<u8>),

    /// The protocol is unrecognized or could not be parsed from the HTTP
    /// request line bytes attached.
    #[error("unrecognized protocol in request line")]
    RequestLineProtocol(Vec<u8>),

    /// The attached bytes are the beginning of the request line, whose length
    /// exceeds the request line limit.
    #[error("request line too long")]
    RequestLineTooLong(Vec<u8>),

    /// The request line contained an invalid target URI.
    #[error("invalid request target URI")]
    RequestTargetUriInvalid(#[from] rhymuri::Error),

    /// An error occurred during string formatting.
    #[error("error during string format")]
    StringFormat,
}
