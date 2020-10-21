//! This crate provides types for handling Hypertext Transfer Protocol (HTTP)
//! 1.1 requests and responses, as specified in [IETF RFC
//! 7230](https://tools.ietf.org/html/rfc7230).
//!
//! To parse a request or response, construct a new [`Request`] or [`Response`]
//! value with [`Request::new`] or [`Response::new`], and then start feeding it
//! input text via [`Request::parse`] or [`Response::parse`].  Each call will
//! consume zero or more of the input characters.  Input is only consumed if
//! enough is provided for the parser to successfully build up another part of
//! the message. Any unused input should be included in the next call along
//! with additional input to continue parsing.  Parsing is complete once the
//! end of the message has been found.
//!
//! To generate a request or response, construct a new value with
//! [`Request::new`] or [`Response::new`], fill in various fields of the value,
//! and emit the final text using [`Request::generate`] or
//! [`Response::generate`].
//!
//! By default, the following constraints are set the lengths of various parts
//! of a [`Request`]:
//!
//! * request line: 1000 bytes
//! * header lines: 1000 bytes each
//! * message overall: 10,000,000 bytes
//!
//! To change or remove the header line length constraint, use the
//! [`MessageHeaders::set_line_limit`] function on the [`headers`] field of
//! [`Request`].  To change or remove the overall message length constraint,
//! set the [`max_message_size`] field of [`Request`].
//!
//! [`headers`]: struct.Request.html#method.generate#structfield.headers
//! [`max_message_size`]: struct.Request.html#structfield.max_message_size
//! [`MessageHeaders::set_line_limit`]: https://docs.rs/rhymessage/1.2.0/rhymessage/struct.MessageHeaders.html#method.set_line_limit
//! [`Response`]: struct.Response.html
//! [`Response::generate`]: struct.Response.html#method.generate
//! [`Response::new`]: struct.Response.html#method.new
//! [`Response::parse`]: struct.Response.html#method.parse
//! [`Request`]: struct.Request.html
//! [`Request::generate`]: struct.Request.html#method.generate
//! [`Request::new`]: struct.Request.html#method.new
//! [`Request::parse`]: struct.Request.html#method.parse

#![warn(clippy::pedantic)]
#![allow(clippy::non_ascii_literal)]
#![warn(missing_docs)]

mod chunked_body;
mod error;
mod request;
mod response;

pub use crate::error::Error;
pub use crate::request::{Request, ParseStatus as RequestParseStatus};
pub use crate::response::{Response, ParseStatus as ResponseParseStatus};

// This is the character sequence corresponding to a carriage return (CR)
// followed by a line feed (LF), which officially delimits each
// line of an HTTP request.
const CRLF: &str = "\r\n";

fn find_crlf<T>(message: T) -> Option<usize>
    where T: AsRef<[u8]>
{
    message.as_ref().windows(2)
        .enumerate()
        .find_map(|(i, window)| if window == b"\r\n" { Some(i) } else { None })
}
