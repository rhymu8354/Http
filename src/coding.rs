//! This module contains helper functions for encoding and decoding
//! the bodies of HTTP requests and responses.  There are two kinds of
//! coding: text coding and content coding.  Content coding changes the
//! bytes of the body in order to represent it differently; for example,
//! to compress the body to a smaller size.  Text coding is how the bytes
//! of the body are interpreted as text, and therefore how it can be
//! converted to/from a Rust string.

use crate::Error;
use flate2::bufread::{
    DeflateDecoder,
    GzDecoder,
};
use rhymessage::MessageHeaders;
use std::io::Read as _;

/// Attempt to reverse any content coding that has been performed on the given
/// message body, as indicated in the given message headers.  The content
/// codings that were performed on the body are listed in the
/// `Content-Encoding` header, in the same order that the encoding was
/// performed.  Therefore, to decode the body, the decoding is performed in
/// reverse order.  Decoding is stopped if any unrecognized coding is
/// encountered, or any error occurs during the decoding process.  Any codings
/// successfully decoded are removed from the `Content-Encoding` header, and
/// the header itself is removed if all codings are decoded.
///
/// # Errors
///
/// [`Error::BadContentEncoding`](enum.Error.html#variant.BadContentEncoding)
/// is returned if an error occurs during the decoding process.
pub fn decode_body<B>(
    headers: &mut MessageHeaders,
    body: B
) -> Result<Vec<u8>, Error>
    where B: AsRef<[u8]>
{
    let mut codings = headers.header_tokens("Content-Encoding");
    let mut body = body.as_ref().to_vec();
    while !codings.is_empty() {
        let coding = codings.pop().unwrap();
        match coding.as_ref() {
            "gzip" => body = gzip_decode(body)?,
            "deflate" => body = deflate_decode(body)?,
            _ => {
                codings.push(coding);
                break;
            },
        };
    }
    if codings.is_empty() {
        headers.remove_header("Content-Encoding");
    } else {
        headers.set_header(
            "Content-Encoding",
            codings.join(", ")
        );
    }
    headers.set_header(
        "Content-Length",
        body.len().to_string()
    );
    Ok(body)
}

/// Attempt to decode the given message body as text.  This will only work if
/// the given headers for the message contain a `Content-Type` header where the
/// type is `text`, the `charset` parameter (`iso-8859-1` is assumed if
/// `charset` is missing) is a text encoding recognized and supported by the
/// [`encoding_rs`](https://crates.io/crates/encoding_rs) crate, and the text
/// is successfully decoded.
#[must_use]
pub fn decode_body_as_text<B>(
    headers: &MessageHeaders,
    body: B
) -> Option<String>
    where B: AsRef<[u8]>
{
    if let Some(content_type) = headers.header_value("Content-Type") {
        let (type_subtype, parameters) = match content_type.find(';') {
            Some(delimiter) => (
                &content_type[..delimiter],
                &content_type[delimiter+1..]
            ),
            None => (&content_type[..], ""),
        };
        if let Some((r#type, _)) = split_at(type_subtype, '/') {
            if !r#type.eq_ignore_ascii_case("text") {
                return None;
            }
            let charset = parameters.split(';')
                .map(str::trim)
                .filter_map(|parameter| split_at(parameter, '='))
                .find_map(|(name, value)| {
                    if name.eq_ignore_ascii_case("charset") {
                        Some(value)
                    } else {
                        None
                    }
                })
                .unwrap_or("iso-8859-1");
            if let Some(encoding) = encoding_rs::Encoding::for_label(charset.as_bytes()) {
                return encoding.decode_without_bom_handling_and_without_replacement(
                    body.as_ref()
                )
                    .map(String::from);
            }
        }
    }
    None
}

fn deflate_decode<B>(body: B) -> Result<Vec<u8>, Error>
    where B: AsRef<[u8]>
{
    let body = body.as_ref();
    let mut decoder = DeflateDecoder::new(body);
    let mut body = Vec::new();
    decoder.read_to_end(&mut body)
        .map_err(Error::BadContentEncoding)?;
    Ok(body)
}

fn gzip_decode<B>(body: B) -> Result<Vec<u8>, Error>
    where B: AsRef<[u8]>
{
    let body = body.as_ref();
    let mut decoder = GzDecoder::new(body);
    let mut body = Vec::new();
    decoder.read_to_end(&mut body)
        .map_err(Error::BadContentEncoding)?;
    Ok(body)
}

fn split_at(
    composite: &str,
    delimiter: char
) -> Option<(&str, &str)> {
    match composite.find(delimiter) {
        Some(delimiter) => Some((
            &composite[..delimiter],
            &composite[delimiter+1..]
        )),
        None => None,
    }
}

#[cfg(test)]
mod tests {

    #![allow(clippy::string_lit_as_bytes)]
    #![allow(clippy::non_ascii_literal)]

    use super::*;

    #[test]
    fn gzip_decode_non_empty_input() {
        let body: &[u8] = &[
            0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x0A, 0xF3, 0x48, 0xCD, 0xC9, 0xC9, 0xD7,
            0x51, 0x08, 0xCF, 0x2F, 0xCA, 0x49, 0x51, 0x04,
            0x00, 0xD0, 0xC3, 0x4A, 0xEC, 0x0D, 0x00, 0x00,
            0x00,
        ];
        let body = gzip_decode(body);
        assert!(body.is_ok());
        let body = body.unwrap();
        assert_eq!("Hello, World!".as_bytes(), body);
    }

    #[test]
    fn gzip_decode_empty_input() {
        let body: &[u8] = &[];
        let body = gzip_decode(body);
        assert!(matches!(
            body,
            Err(Error::BadContentEncoding(_))
        ));
    }

    #[test]
    fn gzip_decode_junk() {
        let body: &[u8] = b"Hello, this is certainly not gzipped data!";
        let body = gzip_decode(body);
        assert!(matches!(
            body,
            Err(Error::BadContentEncoding(_))
        ));
    }

    #[test]
    fn gzip_decode_empty_output() {
        let body: &[u8] = &[
            0x1f, 0x8b, 0x08, 0x08, 0x2d, 0xac, 0xca, 0x5b,
            0x00, 0x03, 0x74, 0x65, 0x73, 0x74, 0x2e, 0x74,
            0x78, 0x74, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00,
        ];
        let body = gzip_decode(body);
        assert!(body.is_ok());
        let body = body.unwrap();
        assert_eq!("".as_bytes(), body);
    }

    #[test]
    fn deflate_decode_non_empty_input() {
        let body: &[u8] = &[
            0xf3, 0x48, 0xcd, 0xc9, 0xc9, 0xd7, 0x51, 0x08,
            0xcf, 0x2f, 0xca, 0x49, 0x51, 0x04, 0x00,
        ];
        let body = deflate_decode(body);
        assert!(body.is_ok());
        let body = body.unwrap();
        assert_eq!("Hello, World!".as_bytes(), body);
    }

    #[test]
    fn deflate_decode_empty_input() {
        let body: &[u8] = &[];
        let body = deflate_decode(body);
        assert!(matches!(
            body,
            Err(Error::BadContentEncoding(_))
        ));
    }

    #[test]
    fn deflate_decode_junk() {
        let body: &[u8] = b"Hello, this is certainly not deflated data!";
        let body = deflate_decode(body);
        assert!(matches!(
            body,
            Err(Error::BadContentEncoding(_))
        ));
    }

    #[test]
    fn deflate_decode_empty_output() {
        let body: &[u8] = &[
            0x03, 0x00,
        ];
        let body = deflate_decode(body);
        assert!(body.is_ok());
        let body = body.unwrap();
        assert_eq!("".as_bytes(), body);
    }

    #[test]
    fn decode_body_not_encoded() {
        let mut headers = MessageHeaders::new();
        let body = b"Hello, World!";
        headers.set_header(
            "Content-Length",
            body.len().to_string()
        );
        assert!(matches!(
            decode_body(&mut headers, body),
            Ok(body) if body == b"Hello, World!"
        ));
    }

    #[test]
    fn decode_body_gzipped() {
        let mut headers = MessageHeaders::new();
        let encoded_body = &[
            0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x0A, 0xF3, 0x48, 0xCD, 0xC9, 0xC9, 0xD7,
            0x51, 0x08, 0xCF, 0x2F, 0xCA, 0x49, 0x51, 0x04,
            0x00, 0xD0, 0xC3, 0x4A, 0xEC, 0x0D, 0x00, 0x00,
            0x00,
        ];
        let decoded_body = b"Hello, World!";
        headers.set_header(
            "Content-Length",
            encoded_body.len().to_string()
        );
        headers.set_header("Content-Encoding", "gzip");
        assert!(matches!(
            decode_body(&mut headers, encoded_body),
            Ok(body) if body == decoded_body
        ));
        assert_eq!(
            decoded_body.len().to_string(),
            headers.header_value("Content-Length").unwrap()
        );
        assert!(!headers.has_header("Content-Encoding"));
    }

    #[test]
    fn decode_body_deflated_then_gzipped() {
        let mut headers = MessageHeaders::new();
        let encoded_body = &[
            0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0xFF, 0xFB, 0xEC, 0x71, 0xF6, 0xE4, 0xC9,
            0xEB, 0x81, 0x1C, 0xE7, 0xF5, 0x4F, 0x79, 0x06,
            0xB2, 0x30, 0x00, 0x00, 0x87, 0x6A, 0xB2, 0x3A,
            0x0F, 0x00, 0x00, 0x00,
        ];
        let decoded_body = b"Hello, World!";
        headers.set_header(
            "Content-Length",
            encoded_body.len().to_string()
        );
        headers.set_header("Content-Encoding", "deflate, gzip");
        assert!(matches!(
            decode_body(&mut headers, encoded_body),
            Ok(body) if body == decoded_body
        ));
        assert_eq!(
            decoded_body.len().to_string(),
            headers.header_value("Content-Length").unwrap()
        );
        assert!(!headers.has_header("Content-Encoding"));
    }

    #[test]
    fn decode_body_unknown_coding_then_gzipped() {
        let mut headers = MessageHeaders::new();
        let encoded_body = &[
            0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x0A, 0xF3, 0x48, 0xCD, 0xC9, 0xC9, 0xD7,
            0x51, 0x08, 0xCF, 0x2F, 0xCA, 0x49, 0x51, 0x04,
            0x00, 0xD0, 0xC3, 0x4A, 0xEC, 0x0D, 0x00, 0x00,
            0x00,
        ];
        let decoded_body = b"Hello, World!";
        headers.set_header(
            "Content-Length",
            encoded_body.len().to_string()
        );
        headers.set_header("Content-Encoding", "foobar, gzip");
        assert!(matches!(
            decode_body(&mut headers, encoded_body),
            Ok(body) if body == decoded_body
        ));
        assert_eq!(
            b"Hello, World!".len().to_string(),
            headers.header_value("Content-Length").unwrap()
        );
        assert_eq!(
            Some("foobar"),
            headers.header_value("Content-Encoding").as_deref()
        );
    }

    #[test]
    fn body_to_string_valid_encoding_iso_8859_1() {
        let mut headers = MessageHeaders::new();
        let body = b"Tickets to Hogwarts leaving from Platform 9\xbe are \xa310 each";
        headers.set_header("Content-Type", "text/plain; charset=iso-8859-1");
        assert_eq!(
            Some("Tickets to Hogwarts leaving from Platform 9¾ are £10 each"),
            decode_body_as_text(&headers, body).as_deref()
        );
    }

    #[test]
    fn body_to_string_valid_encoding_utf_8() {
        let mut headers = MessageHeaders::new();
        let body = "Tickets to Hogwarts leaving from Platform 9¾ are £10 each".as_bytes();
        headers.set_header("Content-Type", "text/plain; charset=utf-8");
        assert_eq!(
            Some("Tickets to Hogwarts leaving from Platform 9¾ are £10 each"),
            decode_body_as_text(&headers, body).as_deref()
        );
    }

    #[test]
    fn body_to_string_invalid_encoding_utf8() {
        let mut headers = MessageHeaders::new();
        let body = b"Tickets to Hogwarts leaving from Platform 9\xbe are \xa310 each";
        headers.set_header("Content-Type", "text/plain; charset=utf-8");
        assert!(decode_body_as_text(&headers, body).is_none());
    }

    #[test]
    fn body_to_string_default_encoding_iso_8859_1() {
        let mut headers = MessageHeaders::new();
        let body = b"Tickets to Hogwarts leaving from Platform 9\xbe are \xa310 each";
        headers.set_header("Content-Type", "text/plain");
        assert_eq!(
            Some("Tickets to Hogwarts leaving from Platform 9¾ are £10 each"),
            decode_body_as_text(&headers, body).as_deref()
        );
    }

}
