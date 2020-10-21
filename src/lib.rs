#![warn(clippy::pedantic)]
#![allow(clippy::non_ascii_literal)]

// TODO: Before publishing to crates.io, remove these and fix the warnings they
// suppress.
//#![warn(missing_docs)]
#![allow(clippy::missing_errors_doc)]
#![allow(dead_code)]

mod chunked_body;
mod error;
mod request;
mod response;

pub use crate::error::Error;
pub use crate::request::Request;
pub use crate::response::Response;

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
