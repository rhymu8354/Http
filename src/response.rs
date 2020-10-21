use rhymessage::MessageHeaders;
use std::io::Write;
use super::chunked_body::{ChunkedBody, DecodeStatus as ChunkedBodyDecodeStatus};
use super::error::Error;
use super::{CRLF, find_crlf};

fn parse_status_line(status_line: &str) -> Result<(usize, &str), Error> {
    // Parse the protocol.
    let protocol_delimiter = status_line.find(' ')
        .ok_or_else(|| Error::StatusLineNoProtocolDelimiter(status_line.into()))?;
    let protocol = &status_line[..protocol_delimiter];
    if protocol != "HTTP/1.1" {
        return Err(Error::StatusLineProtocol(status_line.into()))
    }

    // Parse the status code.
    let status_line_at_status_code = &status_line[protocol_delimiter+1..];
    let status_code_delimiter = status_line_at_status_code.find(' ')
        .ok_or_else(|| Error::StatusLineNoStatusCodeDelimiter(status_line.into()))?;
    let status_code = status_line_at_status_code[..status_code_delimiter].parse::<usize>()
        .map_err(Error::InvalidStatusCode)
        .and_then(|status_code| match status_code {
            status_code if status_code < 1000 => Ok(status_code),
            status_code => Err(Error::StatusCodeOutOfRange(status_code)),
        })?;

    // Parse the reason phrase.
    let reason_phrase = &status_line_at_status_code[status_code_delimiter+1..];
    Ok((status_code, reason_phrase))
}

enum ResponseState {
    ChunkedBody(ChunkedBody),
    FixedBody(usize),
    Headers,
    StatusLine,
}

impl Default for ResponseState {
    fn default() -> Self {
        Self::StatusLine
    }
}

#[derive(Debug, Eq, PartialEq)]
pub enum ParseStatus {
    Complete,
    Incomplete,
}

enum ParseStatusInternal {
    CompletePart,
    CompleteWhole,
    Incomplete,
}

pub struct Response {
    pub body: Vec<u8>,
    pub headers: MessageHeaders,
    pub reason_phrase: std::borrow::Cow<'static, str>,
    state: ResponseState,
    pub status_code: usize,
}

impl Response {
    pub fn generate(&self) -> Result<Vec<u8>, Error> {
        let mut output = Vec::new();
        write!(&mut output, "HTTP/1.1 {} {}\r\n", self.status_code, self.reason_phrase)
            .map_err(|_| Error::StringFormat)?;
        output.append(&mut self.headers.generate().map_err(Error::Headers)?);
        output.extend(&self.body);
        Ok(output)
    }

    #[must_use]
    pub fn new() -> Self {
        Self{
            body: Vec::new(),
            headers: MessageHeaders::new(),
            reason_phrase: "OK".into(),
            state: ResponseState::default(),
            status_code: 200,
        }
    }

    pub fn parse<T>(
        &mut self,
        raw_message: T
    ) -> Result<(ParseStatus, usize), Error>
        where T: AsRef<[u8]>
    {
        let raw_message = raw_message.as_ref();
        let mut total_consumed = 0;
        loop {
            let raw_message_remainder = &raw_message[total_consumed..];
            let state = std::mem::take(&mut self.state);
            let (parse_status, state, consumed) = match state {
                ResponseState::ChunkedBody(chunked_body) => {
                    self.parse_message_for_chunked_body(
                        raw_message_remainder,
                        chunked_body
                    )?
                },
                ResponseState::FixedBody(content_length) => {
                    let (parse_status, consumed) = self.parse_message_for_fixed_body(
                        raw_message_remainder,
                        content_length
                    )?;
                    (parse_status, ResponseState::FixedBody(content_length), consumed)
                },
                ResponseState::Headers => {
                    self.parse_message_for_headers(raw_message_remainder)?
                },
                ResponseState::StatusLine => {
                    self.parse_message_for_status_line(raw_message_remainder)?
                },
            };
            self.state = state;
            total_consumed += consumed;
            match parse_status {
                ParseStatusInternal::CompletePart => (),
                ParseStatusInternal::CompleteWhole => {
                    return Ok((ParseStatus::Complete, total_consumed));
                },
                ParseStatusInternal::Incomplete => {
                    return Ok((ParseStatus::Incomplete, total_consumed));
                }
            };
        }
    }

    fn parse_message_for_chunked_body(
        &mut self,
        raw_message: &[u8],
        mut chunked_body: ChunkedBody,
    ) -> Result<(ParseStatusInternal, ResponseState, usize), Error> {
        match chunked_body.decode(raw_message)? {
            (ChunkedBodyDecodeStatus::Complete, consumed) => {
                self.body = std::mem::take(&mut chunked_body.buffer);
                // TODO: We have to clone here for now because I didn't provide
                // a way to consume the headers into an iterator or take them
                // out.  In the future, once rhymessage::MessageHeaders is
                // improved, we can revisit this code and remove the cloning.
                for header in chunked_body.trailer.headers() {
                    self.headers.add_header(
                        header.name.clone(),
                        header.value.clone()
                    );
                }

                // Now that we've decoded the chunked body, we should remove
                // the "chunked" token from the `Transfer-Encoding` header,
                // and add a `Content-Length` header.
                let mut transfer_encodings = self.headers.header_tokens("Transfer-Encoding");
                transfer_encodings.pop();
                if transfer_encodings.is_empty() {
                    self.headers.remove_header("Transfer-Encoding");
                } else {
                    self.headers.set_header(
                        "Transfer-Encoding",
                        transfer_encodings.join(" ")
                    );
                }
                self.headers.add_header(
                    "Content-Length",
                    self.body.len().to_string()
                );
                self.headers.remove_header("Trailer");
                Ok((
                    ParseStatusInternal::CompleteWhole,
                    ResponseState::default(),
                    consumed
                ))
            },
            (ChunkedBodyDecodeStatus::Incomplete, consumed) => {
                Ok((
                    ParseStatusInternal::Incomplete,
                    ResponseState::ChunkedBody(chunked_body),
                    consumed
                ))
            },
        }
    }

    fn parse_message_for_fixed_body(
        &mut self,
        raw_message: &[u8],
        content_length: usize,
    ) -> Result<(ParseStatusInternal, usize), Error> {
        let needed = content_length - self.body.len();
        if raw_message.len() >= needed {
            self.body.extend(&raw_message[..needed]);
            Ok((ParseStatusInternal::CompleteWhole, needed))
        } else {
            self.body.extend(raw_message);
            Ok((ParseStatusInternal::Incomplete, raw_message.len()))
        }
    }

    fn parse_message_for_headers(
        &mut self,
        raw_message: &[u8]
    ) -> Result<(ParseStatusInternal, ResponseState, usize), Error> {
        let parse_results = self.headers.parse(raw_message)
            .map_err(Error::Headers)?;
        match parse_results.status {
            rhymessage::ParseStatus::Complete => {
                if let Some(content_length) = self.headers.header_value("Content-Length") {
                    let content_length = content_length.parse::<usize>()
                        .map_err(Error::InvalidContentLength)?;
                    self.body.reserve(content_length);
                    Ok((
                        ParseStatusInternal::CompletePart,
                        ResponseState::FixedBody(content_length),
                        parse_results.consumed
                    ))
                } else if self.headers.has_header_token("Transfer-Encoding", "chunked") {
                    Ok((
                        ParseStatusInternal::CompletePart,
                        ResponseState::ChunkedBody(ChunkedBody::new()),
                        parse_results.consumed
                    ))
                } else {
                    Ok((
                        ParseStatusInternal::CompleteWhole,
                        ResponseState::Headers,
                        parse_results.consumed
                    ))
                }
            },
            rhymessage::ParseStatus::Incomplete => {
                Ok((
                    ParseStatusInternal::Incomplete,
                    ResponseState::Headers,
                    parse_results.consumed
                ))
            },
        }
    }

    fn parse_message_for_status_line(
        &mut self,
        raw_message: &[u8]
    ) -> Result<(ParseStatusInternal, ResponseState, usize), Error> {
        match find_crlf(raw_message) {
            Some(status_line_end) => {
                let status_line = &raw_message[0..status_line_end];
                let status_line = std::str::from_utf8(status_line)
                    .map_err(|_| Error::StatusLineNotValidText(status_line.to_vec()))?;
                let consumed = status_line_end + CRLF.len();
                let (status_code, reason_phrase) = parse_status_line(status_line)?;
                self.status_code = status_code;
                self.reason_phrase = reason_phrase.to_string().into();
                Ok((
                    ParseStatusInternal::CompletePart,
                    ResponseState::Headers,
                    consumed
                ))
            },
            None => Ok((
                ParseStatusInternal::Incomplete,
                ResponseState::StatusLine,
                0
            )),
        }
    }
}

impl Default for Response {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {

    use super::*;

    #[test]
    fn generate_get_response() {
        let mut response = Response::new();
        response.status_code = 200;
        response.reason_phrase = "OK".into();
        response.headers.add_header("Date", "Mon, 27 Jul 2009 12:28:53 GMT");
        response.headers.add_header("Accept-Ranges", "bytes");
        response.headers.add_header("Content-Type", "text/plain");
        response.body = "Hello World! My payload includes a trailing CRLF.\r\n".into();
        response.headers.add_header(
            "Content-Length",
            format!("{}", response.body.len())
        );
        assert_eq!(
            Ok(format!(
                concat!(
                    "HTTP/1.1 200 OK\r\n",
                    "Date: Mon, 27 Jul 2009 12:28:53 GMT\r\n",
                    "Accept-Ranges: bytes\r\n",
                    "Content-Type: text/plain\r\n",
                    "Content-Length: {}\r\n",
                    "\r\n",
                    "Hello World! My payload includes a trailing CRLF.\r\n",
                ),
                response.body.len()
            ).as_bytes()),
            response.generate().as_deref()
        );
    }

    #[test]
    fn parse_get_response_with_body_and_content_length() {
        let raw_response = concat!(
            "HTTP/1.1 200 OK\r\n",
            "Date: Mon, 27 Jul 2009 12:28:53 GMT\r\n",
            "Server: Apache\r\n",
            "Last-Modified: Wed, 22 Jul 2009 19:15:56 GMT\r\n",
            "ETag: \"34aa387-d-1568eb00\"\r\n",
            "Accept-Ranges: bytes\r\n",
            "Content-Length: 51\r\n",
            "Vary: Accept-Encoding\r\n",
            "Content-Type: text/plain\r\n",
            "\r\n",
            "Hello World! My payload includes a trailing CRLF.\r\n",
        );
        let mut response = Response::new();
        assert_eq!(
            Ok((ParseStatus::Complete, raw_response.len())),
            response.parse(raw_response)
        );
        assert_eq!(200, response.status_code);
        assert_eq!("OK", response.reason_phrase);
        assert!(response.headers.has_header("Date"));
        assert_eq!(
            Some("Mon, 27 Jul 2009 12:28:53 GMT"),
            response.headers.header_value("Date").as_deref()
        );
        assert!(response.headers.has_header("Accept-Ranges"));
        assert_eq!(
            Some("bytes"),
            response.headers.header_value("Accept-Ranges").as_deref()
        );
        assert!(response.headers.has_header("Content-Type"));
        assert_eq!(
            Some("text/plain"),
            response.headers.header_value("Content-Type").as_deref()
        );
        assert!(response.headers.has_header("Content-Length"));
        assert_eq!(
            Some("51"),
            response.headers.header_value("Content-Length").as_deref()
        );
        assert_eq!(
            "Hello World! My payload includes a trailing CRLF.\r\n".as_bytes(),
            response.body
        );
    }

    #[test]
    fn parse_get_response_with_chunked_body_no_other_transfer_coding() {
        let raw_response = concat!(
            "HTTP/1.1 200 OK\r\n",
            "Date: Mon, 27 Jul 2009 12:28:53 GMT\r\n",
            "Server: Apache\r\n",
            "Last-Modified: Wed, 22 Jul 2009 19:15:56 GMT\r\n",
            "ETag: \"34aa387-d-1568eb00\"\r\n",
            "Accept-Ranges: bytes\r\n",
            "Transfer-Encoding: chunked\r\n",
            "Vary: Accept-Encoding\r\n",
            "Content-Type: text/plain\r\n",
            "Trailer: X-Foo\r\n",
            "\r\n",
            "C\r\n",
            "Hello World!\r\n",
            "16\r\n",
            " My payload includes a\r\n",
            "11\r\n",
            " trailing CRLF.\r\n\r\n",
            "0\r\n",
            "X-Foo: Bar\r\n",
            "\r\n",
        );
        let mut response = Response::new();
        assert_eq!(
            Ok((ParseStatus::Complete, raw_response.len())),
            response.parse(raw_response)
        );
        assert_eq!(200, response.status_code);
        assert_eq!("OK", response.reason_phrase);
        assert!(response.headers.has_header("Date"));
        assert_eq!(
            Some("Mon, 27 Jul 2009 12:28:53 GMT"),
            response.headers.header_value("Date").as_deref()
        );
        assert!(response.headers.has_header("Accept-Ranges"));
        assert_eq!(
            Some("bytes"),
            response.headers.header_value("Accept-Ranges").as_deref()
        );
        assert!(response.headers.has_header("Content-Type"));
        assert_eq!(
            Some("text/plain"),
            response.headers.header_value("Content-Type").as_deref()
        );
        assert_eq!(
            Some("Bar"),
            response.headers.header_value("X-Foo").as_deref()
        );
        assert_eq!(
            Some("51"),
            response.headers.header_value("Content-Length").as_deref()
        );
        assert_eq!(
            "Hello World! My payload includes a trailing CRLF.\r\n".as_bytes(),
            response.body
        );
        assert!(!response.headers.has_header_token("Transfer-Encoding", "chunked"));
        assert!(!response.headers.has_header("Trailer"));
        assert!(!response.headers.has_header("Transfer-Encoding"));
        assert_eq!(
            "Hello World! My payload includes a trailing CRLF.\r\n".as_bytes(),
            response.body
        );
    }

    #[test]
    fn parse_get_response_with_chunked_body_with_other_transfer_coding() {
        let raw_response = concat!(
            "HTTP/1.1 200 OK\r\n",
            "Date: Mon, 27 Jul 2009 12:28:53 GMT\r\n",
            "Transfer-Encoding: foobar, chunked\r\n",
            "Content-Type: text/plain\r\n",
            "\r\n",
            "0\r\n",
            "\r\n",
        );
        let mut response = Response::new();
        assert_eq!(
            Ok((ParseStatus::Complete, raw_response.len())),
            response.parse(raw_response)
        );
        assert_eq!(
            Some("foobar"),
            response.headers.header_value("Transfer-Encoding").as_deref()
        );
    }

    #[test]
    fn parse_incomplete_body_response() {
        let raw_response = concat!(
            "HTTP/1.1 200 OK\r\n",
            "Date: Mon, 27 Jul 2009 12:28:53 GMT\r\n",
            "Server: Apache\r\n",
            "Last-Modified: Wed, 22 Jul 2009 19:15:56 GMT\r\n",
            "ETag: \"34aa387-d-1568eb00\"\r\n",
            "Accept-Ranges: bytes\r\n",
            "Content-Length: 52\r\n",
            "Vary: Accept-Encoding\r\n",
            "Content-Type: text/plain\r\n",
            "\r\n",
            "Hello World! My payload includes a trailing CRLF.\r\n",
        );
        let mut response = Response::new();
        assert_eq!(
            Ok((ParseStatus::Incomplete, raw_response.len())),
            response.parse(raw_response)
        );
    }

    #[test]
    fn parse_incomplete_headers_between_lines_response() {
        let raw_response_first_part = concat!(
            "HTTP/1.1 200 OK\r\n",
            "Date: Mon, 27 Jul 2009 12:28:53 GMT\r\n",
            "Server: Apache\r\n",
            "Last-Modified: Wed, 22 Jul 2009 19:15:56 GMT\r\n",
        );
        let raw_response = String::from(raw_response_first_part)
            + "ETag: \"34aa387-d-1568eb00\"\r\n";
        let mut response = Response::new();
        assert_eq!(
            Ok((ParseStatus::Incomplete, raw_response_first_part.len())),
            response.parse(raw_response)
        );
    }

    #[test]
    fn parse_incomplete_headers_mid_line_response() {
        let raw_response_first_part = concat!(
            "HTTP/1.1 200 OK\r\n",
            "Date: Mon, 27 Jul 2009 12:28:53 GMT\r\n",
        );
        let raw_response = String::from(raw_response_first_part)
            + "Server: Apache\r\n"
            + "Last-Modified: Wed, 22 Ju";
        let mut response = Response::new();
        assert_eq!(
            Ok((ParseStatus::Incomplete, raw_response_first_part.len())),
            response.parse(raw_response)
        );
    }

    #[test]
    fn parse_incomplete_status_line() {
        let raw_response = "HTTP/1.1 200 OK\r";
        let mut response = Response::new();
        assert_eq!(
            Ok((ParseStatus::Incomplete, 0)),
            response.parse(raw_response)
        );
    }

    #[test]
    fn parse_no_headers_response() {
        let raw_response = "HTTP/1.1 200 OK\r\n";
        let mut response = Response::new();
        assert_eq!(
            Ok((ParseStatus::Incomplete, raw_response.len())),
            response.parse(raw_response)
        );
    }

    #[test]
    fn parse_invalid_response_no_protocol() {
        let raw_response = " 200 OK\r\n";
        let mut response = Response::new();
        assert_eq!(
            Err(Error::StatusLineProtocol(" 200 OK".into())),
            response.parse(raw_response),
        );
    }

    #[test]
    fn parse_invalid_response_no_status_code() {
        let raw_response = "HTTP/1.1  OK\r\n";
        let mut response = Response::new();
        assert!(matches!(
            response.parse(raw_response),
            Err(Error::InvalidStatusCode(_))
        ));
    }

    #[test]
    fn parse_invalid_response_no_reason_phrase() {
        let raw_response = "HTTP/1.1 200\r\n";
        let mut response = Response::new();
        assert_eq!(
            Err(Error::StatusLineNoStatusCodeDelimiter("HTTP/1.1 200".into())),
            response.parse(raw_response),
        );
    }

    #[test]
    fn parse_invalid_damaged_header() {
        let raw_response = concat!(
            "HTTP/1.1 200 OK\r\n",
            "Date: Mon, 27 Jul 2009 12:28:53 GMT\r\n",
            "Server: Apache\r\n",
            "Last-Modified Wed, 22 Jul 2009 19:15:56 GMT\r\n",
            "ETag: \"34aa387-d-1568eb00\"\r\n",
            "Accept-Ranges: bytes\r\n",
            "Content-Length: 51\r\n",
            "Vary: Accept-Encoding\r\n",
            "Content-Type: text/plain\r\n",
            "\r\n",
            "Hello World! My payload includes a trailing CRLF.\r\n",
        );
        let mut response = Response::new();
        assert_eq!(
            Err(Error::Headers(
                rhymessage::Error::HeaderNameContainsIllegalCharacter(
                    "Last-Modified Wed, 22 Jul 2009 19".into()
                )
            )),
            response.parse(raw_response),
        );
    }

    #[test]
    fn response_with_no_content_length_or_chunked_transfer_encoding_has_no_body() {
        let raw_response = concat!(
            "HTTP/1.1 200 OK\r\n",
            "Date: Mon, 27 Jul 2009 12:28:53 GMT\r\n",
            "Server: Apache\r\n",
            "Last-Modified: Wed, 22 Jul 2009 19:15:56 GMT\r\n",
            "ETag: \"34aa387-d-1568eb00\"\r\n",
            "Accept-Ranges: bytes\r\n",
            "Vary: Accept-Encoding\r\n",
            "Content-Type: text/plain\r\n",
            "\r\n",
        );
        let trailer = "Hello World! My payload includes a trailing CRLF.\r\n";
        let mut response = Response::new();
        assert_eq!(
            Ok((ParseStatus::Complete, raw_response.len())),
            response.parse(String::from(raw_response) + trailer),
        );
        assert!(response.body.is_empty());
    }

}
