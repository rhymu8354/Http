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

    /// The status code in the status line is not valid.
    #[error("invalid status code")]
    InvalidStatusCode(std::num::ParseIntError),

    /// The message is too large to fit within the configured size limit.
    #[error("message exceeds maximum size limit")]
    MessageTooLong,

    /// No delimiter was found to parse the method from the attached HTTP
    /// request line.
    #[error("unable to find method delimiter in request line")]
    RequestLineNoMethodDelimiter(String),

    /// The method could not be parsed from the HTTP request line attached.
    /// Either there is extra whitespace, or the method is an empty string.
    #[error("unable to parse method from request line")]
    RequestLineNoMethodOrExtraWhitespace(String),

    /// No delimiter was found to parse the target URI from the attached HTTP
    /// request line.
    #[error("unable to find target URI delimiter in request line")]
    RequestLineNoTargetDelimiter(String),

    /// The target URI could not be parsed from the HTTP request line attached.
    /// Either there is extra whitespace, or the target URI is an empty string.
    #[error("unable to parse target URI from request line")]
    RequestLineNoTargetOrExtraWhitespace(String),

    /// The attached bytes did not parse as valid text for the HTTP request
    /// line.
    #[error("request line is not valid text")]
    RequestLineNotValidText(Vec<u8>),

    /// The protocol is unrecognized or could not be parsed from the HTTP
    /// request line attached.
    #[error("unrecognized protocol in request line")]
    RequestLineProtocol(String),

    /// The attached bytes are the beginning of the request line, whose length
    /// exceeds the request line limit.
    #[error("request line too long")]
    RequestLineTooLong(Vec<u8>),

    /// The request line contained an invalid target URI.
    #[error("invalid request target URI")]
    RequestTargetUriInvalid(#[from] rhymuri::Error),

    /// The attached status code was out of range.
    #[error("status code is out of range")]
    StatusCodeOutOfRange(usize),

    /// No delimiter was found to parse the protocol from the attached HTTP
    /// status line.
    #[error("unable to find protocol delimiter in status line")]
    StatusLineNoProtocolDelimiter(String),

    /// No delimiter was found to parse the status code from the attached HTTP
    /// status line.
    #[error("unable to parse status code from status line")]
    StatusLineNoStatusCodeDelimiter(String),

    /// The attached bytes did not parse as valid text for the HTTP status
    /// line.
    #[error("status line is not valid text")]
    StatusLineNotValidText(Vec<u8>),

    /// The protocol is unrecognized or could not be parsed from the HTTP
    /// status line attached.
    #[error("unrecognized protocol in status line")]
    StatusLineProtocol(String),

    /// An error occurred during string formatting.
    #[error("error during string format")]
    StringFormat,
}
