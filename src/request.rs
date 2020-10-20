use rhymessage::MessageHeaders;
use rhymuri::Uri;
use std::io::Write;
use super::error::Error;
use super::CRLF;

fn find_whitespace<T>(message: T) -> Option<usize>
    where T: AsRef<[u8]>
{
    let message = message.as_ref();
    match message.len() {
        0 | 1 => None,
        len => {
            for i in 0..len-1 {
                if
                    message[i] == b'\r'
                    && message[i+1] == b'\n'
                {
                    return Some(i)
                }
            }
            None
        }
    }
}

fn parse_request_line(request_line: &str) -> Result<(&str, Uri), Error> {
    // Parse the method.
    let method_delimiter = request_line.find(' ')
        .ok_or_else(|| Error::RequestLineInvalid(request_line.into()))?;
    let method = &request_line[0..method_delimiter];
    if method.is_empty() {
        return Err(Error::RequestLineNoMethodOrExtraWhitespace(request_line.into()));
    }

    // Parse the target URI.
    let request_line_at_target = &request_line[method_delimiter+1..];
    let target_delimiter = request_line_at_target.find(' ')
        .ok_or_else(|| Error::RequestLineInvalid(request_line.into()))?;
    if target_delimiter == 0 {
        return Err(Error::RequestLineNoTargetOrExtraWhitespace(request_line.into()));
    }
    let target = Uri::parse(&request_line_at_target[..target_delimiter])?;

    // Parse the protocol.
    let request_line_at_protocol = &request_line_at_target[target_delimiter+1..];
    if request_line_at_protocol == "HTTP/1.1" {
        Ok((method, target))
    } else {
        Err(Error::RequestLineProtocol(request_line.into()))
    }
}

#[derive(Debug, Eq, PartialEq)]
enum RequestState {
    Body(usize),
    Headers,
    RequestLine,
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

pub struct Request {
    pub body: Vec<u8>,
    pub headers: MessageHeaders,
    pub max_message_size: Option<usize>,
    pub method: std::borrow::Cow<'static, str>,
    pub request_line_limit: Option<usize>,
    state: RequestState,
    pub target: Uri,
    total_bytes: usize,
}

impl Request {
    fn count_bytes(&mut self, bytes: usize) -> Result<(), Error> {
        self.total_bytes += bytes;
        match self.max_message_size {
            Some(max_message_size) if self.total_bytes > max_message_size => {
                Err(Error::MessageTooLong)
            },
            _ => Ok(())
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
            let (parse_status, consumed) = match self.state {
                RequestState::Body(content_length) => {
                    self.parse_message_for_body(raw_message_remainder, content_length)?
                },
                RequestState::Headers => {
                    self.parse_message_for_headers(raw_message_remainder)?
                },
                RequestState::RequestLine => {
                    self.parse_message_for_request_line(raw_message_remainder)?
                },
            };
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

    fn parse_message_for_body(
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
    ) -> Result<(ParseStatusInternal, usize), Error> {
        let parse_results = self.headers.parse(raw_message)?;
        self.count_bytes(parse_results.consumed)?;
        match parse_results.status {
            rhymessage::ParseStatus::Complete => {
                if let Some(content_length) = self.headers.header_value("Content-Length") {
                    let content_length = content_length.parse::<usize>()
                        .map_err(Error::InvalidContentLength)?;
                    self.count_bytes(content_length)?;
                    self.body.reserve(content_length);
                    self.state = RequestState::Body(content_length);
                    Ok((ParseStatusInternal::CompletePart, parse_results.consumed))
                } else {
                    Ok((ParseStatusInternal::CompleteWhole, parse_results.consumed))
                }
            },
            rhymessage::ParseStatus::Incomplete => {
                Ok((ParseStatusInternal::Incomplete, parse_results.consumed))
            },
        }
    }

    fn parse_message_for_request_line(
        &mut self,
        raw_message: &[u8]
    ) -> Result<(ParseStatusInternal, usize), Error> {
        match (find_whitespace(raw_message), self.request_line_limit) {
            (Some(request_line_end), Some(limit)) if request_line_end > limit => {
                Err(Error::RequestLineTooLong(raw_message[..limit].to_vec()))
            },
            (Some(request_line_end), _) => {
                let request_line = &raw_message[0..request_line_end];
                let request_line = std::str::from_utf8(request_line)
                    .map_err(|_| Error::RequestLineInvalid(request_line.to_vec()))?;
                let consumed = request_line_end + CRLF.len();
                self.count_bytes(consumed)?;
                self.state = RequestState::Headers;
                let (method, target) = parse_request_line(request_line)?;
                self.method = method.to_string().into();
                self.target = target;
                Ok((ParseStatusInternal::CompletePart, consumed))
            },
            (None, Some(limit)) if raw_message.len() > limit => {
                Err(Error::RequestLineTooLong(raw_message[..limit].to_vec()))
            },
            (None, _) => Ok((ParseStatusInternal::Incomplete, 0)),
        }
    }

    pub fn generate(&self) -> Result<Vec<u8>, Error> {
        let mut output = Vec::new();
        write!(&mut output, "{} {} HTTP/1.1\r\n", self.method, self.target)
            .map_err(|_| Error::StringFormat)?;
        output.append(&mut self.headers.generate()?);
        output.extend(&self.body);
        Ok(output)
    }

    #[must_use]
    pub fn new() -> Self {
        let mut request = Self{
            body: Vec::new(),
            headers: MessageHeaders::new(),
            max_message_size: Some(10_000_000),
            method: "GET".into(),
            request_line_limit: Some(1000),
            state: RequestState::RequestLine,
            target: Uri::default(),
            total_bytes: 0,
        };
        request.headers.set_line_limit(Some(1000));
        request
    }
}

impl Default for Request {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {

    use super::*;

    #[test]
    fn generate_get_request() {
        let mut request = Request::new();
        request.method = "GET".into();
        request.target = Uri::parse("/foo").unwrap();
        request.headers.set_header("Host", "www.example.com");
        request.headers.set_header("Content-Type", "text/plain");
        assert_eq!(
            Ok(concat!(
                "GET /foo HTTP/1.1\r\n",
                "Host: www.example.com\r\n",
                "Content-Type: text/plain\r\n",
                "\r\n",
            ).as_bytes()),
            request.generate().as_deref()
        );
    }

    #[test]
    fn generate_put_request() {
        let mut request = Request::new();
        request.method = "PUT".into();
        request.target = Uri::parse("/foo").unwrap();
        request.headers.set_header("Host", "www.example.com");
        request.headers.set_header("Content-Type", "text/plain");
        request.body = "FeelsGoodMan".into();
        request.headers.add_header(
            "Content-Length",
            format!("{}", request.body.len())
        );
        assert_eq!(
            Ok(format!(
                concat!(
                    "PUT /foo HTTP/1.1\r\n",
                    "Host: www.example.com\r\n",
                    "Content-Type: text/plain\r\n",
                    "Content-Length: {}\r\n",
                    "\r\n",
                    "FeelsGoodMan",
                ),
                request.body.len()
            ).as_bytes()),
            request.generate().as_deref()
        );
    }

    #[test]
    fn parse_get_request_ascii_target_uri() {
        let mut request = Request::new();
        let raw_request = concat!(
            "GET /hello.txt HTTP/1.1\r\n",
            "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n",
            "Host: www.example.com\r\n",
            "Accept-Language: en, mi\r\n",
            "\r\n",
        );
        assert_eq!(
            Ok((ParseStatus::Complete, raw_request.len())),
            request.parse(raw_request)
        );
        assert_eq!("GET", request.method);
        assert_eq!("/hello.txt", request.target.to_string());
        assert!(request.headers.has_header("User-Agent"));
        assert_eq!(
            Some("curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3"),
            request.headers.header_value("User-Agent").as_deref()
        );
        assert!(request.headers.has_header("Host"));
        assert_eq!(
            Some("www.example.com"),
            request.headers.header_value("Host").as_deref()
        );
        assert!(request.headers.has_header("Accept-Language"));
        assert_eq!(
            Some("en, mi"),
            request.headers.header_value("Accept-Language").as_deref()
        );
        assert!(request.body.is_empty());
    }

    #[test]
    fn parse_get_request_non_ascii_target_uri() {
        let mut request = Request::new();
        let raw_request = concat!(
            "GET /%F0%9F%92%A9.txt HTTP/1.1\r\n",
            "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n",
            "Host: www.example.com\r\n",
            "Accept-Language: en, mi\r\n",
            "\r\n",
        );
        assert_eq!(
            Ok((ParseStatus::Complete, raw_request.len())),
            request.parse(raw_request)
        );
        assert_eq!("GET", request.method);
        let mut expected_uri = Uri::default();
        expected_uri.set_path(
            ["", "💩.txt"].iter()
                .map(|segment| segment.as_bytes().to_vec())
                .collect::<Vec<_>>()
        );
        assert_eq!(expected_uri, request.target);
        assert!(request.headers.has_header("User-Agent"));
        assert_eq!(
            Some("curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3"),
            request.headers.header_value("User-Agent").as_deref()
        );
        assert!(request.headers.has_header("Host"));
        assert_eq!(
            Some("www.example.com"),
            request.headers.header_value("Host").as_deref()
        );
        assert!(request.headers.has_header("Accept-Language"));
        assert_eq!(
            Some("en, mi"),
            request.headers.header_value("Accept-Language").as_deref()
        );
        assert!(request.body.is_empty());
    }

    #[test]
    fn parse_post_request() {
        let raw_request_body = "say=Hi&to=Mom";
        let raw_request_extra = "\r\n";
        let raw_request_headers = format!(
            concat!(
                "POST / HTTP/1.1\r\n",
                "Host: foo.com\r\n",
                "Content-Type: application/x-www-form-urlencoded\r\n",
                "Content-Length: {}\r\n",
                "\r\n",
            ),
            raw_request_body.len()
        );
        let mut request = Request::new();
        assert_eq!(
            Ok((
                ParseStatus::Complete,
                raw_request_headers.len() + raw_request_body.len()
            )),
            request.parse(
                raw_request_headers
                + raw_request_body
                + raw_request_extra
            )
        );
        assert_eq!("POST", request.method);
        assert_eq!("/", request.target.to_string());
        assert!(request.headers.has_header("Content-Type"));
        assert_eq!(
            Some("application/x-www-form-urlencoded"),
            request.headers.header_value("Content-Type").as_deref()
        );
        assert!(request.headers.has_header("Host"));
        assert_eq!(
            Some("foo.com"),
            request.headers.header_value("Host").as_deref()
        );
        assert!(request.headers.has_header("Content-Length"));
        assert_eq!(
            Some(format!("{}", raw_request_body.len())),
            request.headers.header_value("Content-Length")
        );
        assert_eq!(
            raw_request_body.as_bytes(),
            request.body
        );
    }

    #[test]
    fn parse_invalid_request_no_method() {
        let raw_request = concat!(
            " /hello.txt HTTP/1.1\r\n",
            "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n",
            "Host: www.example.com\r\n",
            "Accept-Language: en, mi\r\n",
            "\r\n",
        );
        let mut request = Request::new();
        assert_eq!(
            Err(Error::RequestLineNoMethodOrExtraWhitespace(b" /hello.txt HTTP/1.1".to_vec())),
            request.parse(raw_request)
        );
    }

    #[test]
    fn parse_invalid_request_no_target() {
        let raw_request = concat!(
            "GET  HTTP/1.1\r\n",
            "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n",
            "Host: www.example.com\r\n",
            "Accept-Language: en, mi\r\n",
            "\r\n",
        );
        let mut request = Request::new();
        assert_eq!(
            Err(Error::RequestLineNoTargetOrExtraWhitespace(b"GET  HTTP/1.1".to_vec())),
            request.parse(raw_request)
        );
    }

    #[test]
    fn parse_invalid_request_no_protocol() {
        let raw_request = concat!(
            "GET /hello.txt\r\n",
            "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n",
            "Host: www.example.com\r\n",
            "Accept-Language: en, mi\r\n",
            "\r\n",
        );
        let mut request = Request::new();
        assert_eq!(
            Err(Error::RequestLineInvalid(b"GET /hello.txt".to_vec())),
            request.parse(raw_request)
        );
    }

    #[test]
    fn parse_invalid_request_empty_protocol() {
        let raw_request = concat!(
            "GET /hello.txt \r\n",
            "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n",
            "Host: www.example.com\r\n",
            "Accept-Language: en, mi\r\n",
            "\r\n",
        );
        let mut request = Request::new();
        assert_eq!(
            Err(Error::RequestLineProtocol(b"GET /hello.txt ".to_vec())),
            request.parse(raw_request)
        );
    }

    #[test]
    fn parse_invalid_request_bad_protocol() {
        let raw_request = concat!(
            "GET /hello.txt FOO\r\n",
            "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n",
            "Host: www.example.com\r\n",
            "Accept-Language: en, mi\r\n",
            "\r\n",
        );
        let mut request = Request::new();
        assert_eq!(
            Err(Error::RequestLineProtocol(b"GET /hello.txt FOO".to_vec())),
            request.parse(raw_request)
        );
    }

    #[test]
    fn parse_invalid_damaged_header() {
        let raw_request = concat!(
            "GET /hello.txt HTTP/1.1\r\n",
            "User-Agent curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n",
            "Host: www.example.com\r\n",
            "Accept-Language: en, mi\r\n",
            "\r\n",
        );
        let mut request = Request::new();
        assert_eq!(
            Err(Error::Headers(
                rhymessage::Error::HeaderLineMissingColon(
                    String::from("User-Agent curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3")
                )
            )),
            request.parse(raw_request)
        );
    }

    #[test]
    fn parse_invalid_header_line_too_long() {
        let test_header_name = "X-Poggers";
        let test_header_name_with_delimiters = String::from(test_header_name) + ": ";
        let value_is_too_long = "X".repeat(
            999 - test_header_name_with_delimiters.len()
        );
        let too_long_header = test_header_name_with_delimiters
            + &value_is_too_long + "\r\n";
        let raw_request = concat!(
            "GET /hello.txt HTTP/1.1\r\n",
            "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n",
        ).to_string()
            + &too_long_header
            + "Host: www.example.com\r\n"
            + "Accept-Language: en, mi\r\n"
            + "\r\n";
        let mut request = Request::new();
        assert_eq!(
            Err(Error::Headers(rhymessage::Error::HeaderLineTooLong(
                too_long_header[0..1000].as_bytes().to_vec()
            ))),
            request.parse(raw_request)
        );
    }

    #[test]
    fn parse_valid_header_line_longer_than_default() {
        let test_header_name = "X-Poggers";
        let test_header_name_with_delimiters = String::from(test_header_name) + ": ";
        let value_is_long_but_within_custom_limit = "X".repeat(
            999 - test_header_name_with_delimiters.len()
        );
        let raw_request = concat!(
            "GET /hello.txt HTTP/1.1\r\n",
            "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n",
        ).to_string()
            + &test_header_name_with_delimiters
            + &value_is_long_but_within_custom_limit + "\r\n"
            + "Host: www.example.com\r\n"
            + "Accept-Language: en, mi\r\n"
            + "\r\n";
        let mut request = Request::new();
        request.headers.set_line_limit(Some(1001));
        assert_eq!(
            Ok((ParseStatus::Complete, raw_request.len())),
            request.parse(raw_request)
        );
    }

    #[test]
    fn parse_invalid_body_insanely_too_large() {
        let raw_request = concat!(
            "POST /hello.txt HTTP/1.1\r\n",
            "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n",
            "Host: www.example.com\r\n",
            "Content-Length: 1000000000000000000000000000000000000000000000000000000000000000000\r\n",
            "Accept-Language: en, mi\r\n",
            "\r\n",
        );
        let mut request = Request::new();
        assert!(matches!(
            request.parse(raw_request),
            Err(Error::InvalidContentLength(std::num::ParseIntError{..}))
        ));
    }

    #[test]
    fn parse_invalid_body_slightly_too_large() {
        let raw_request = concat!(
            "POST /hello.txt HTTP/1.1\r\n",
            "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n",
            "Host: www.example.com\r\n",
            "Content-Length: 10000001\r\n",
            "Accept-Language: en, mi\r\n",
            "\r\n",
        );
        let mut request = Request::new();
        assert_eq!(
            Err(Error::MessageTooLong),
            request.parse(raw_request)
        );
    }

    #[test]
    fn parse_incomplete_body_request() {
        let raw_request = concat!(
            "POST / HTTP/1.1\r\n",
            "Host: foo.com\r\n",
            "Content-Type: application/x-www-form-urlencoded\r\n",
            "Content-Length: 100\r\n",
            "\r\n",
            "say=Hi&to=Mom\r\n",
        );
        let mut request = Request::new();
        assert_eq!(
            Ok((ParseStatus::Incomplete, raw_request.len())),
            request.parse(raw_request)
        );
    }

    #[test]
    fn parse_incomplete_headers_between_lines_request() {
        let raw_request_first_part = concat!(
            "POST / HTTP/1.1\r\n",
            "Host: foo.com\r\n",
        );
        let raw_request = String::from(raw_request_first_part)
            + "Content-Type: application/x-www-form-urlencoded\r\n";
        let mut request = Request::new();
        assert_eq!(
            Ok((ParseStatus::Incomplete, raw_request_first_part.len())),
            request.parse(raw_request)
        );
    }

    #[test]
    fn parse_incomplete_headers_mid_line_request() {
        let raw_request_first_part = concat!(
            "POST / HTTP/1.1\r\n",
        );
        let raw_request = String::from(raw_request_first_part)
            + "Host: foo.com\r\n"
            + "Content-Type: application/x-w";
        let mut request = Request::new();
        assert_eq!(
            Ok((ParseStatus::Incomplete, raw_request_first_part.len())),
            request.parse(raw_request)
        );
    }

    #[test]
    fn parse_incomplete_request_line() {
        let raw_request = "POST / HTTP/1.1\r";
        let mut request = Request::new();
        assert_eq!(
            Ok((ParseStatus::Incomplete, 0)),
            request.parse(raw_request)
        );
    }

    #[test]
    fn parse_incomplete_no_headers_request() {
        let raw_request = "POST / HTTP/1.1\r\n";
        let mut request = Request::new();
        assert_eq!(
            Ok((ParseStatus::Incomplete, raw_request.len())),
            request.parse(raw_request)
        );
    }

    #[test]
    fn request_with_no_content_length_or_chunked_transfer_encoding_has_no_body() {
        let raw_request = concat!(
            "GET /hello.txt HTTP/1.1\r\n",
            "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n",
            "Host: www.example.com\r\n",
            "Accept-Language: en, mi\r\n",
            "\r\n",
        );
        let raw_request_with_extra = String::from(raw_request)
             + "Hello, World!\r\n";
        let mut request = Request::new();
        assert_eq!(
            Ok((ParseStatus::Complete, raw_request.len())),
            request.parse(raw_request_with_extra)
        );
        assert!(request.body.is_empty());
    }

    #[test]
    fn parse_invalid_request_line_too_long() {
        let uri_too_long = "X".repeat(1000);
        let raw_request = String::from("GET ")
            + &uri_too_long + " HTTP/1.1\r\n";
        let mut request = Request::new();
        assert_eq!(
            Err(Error::RequestLineTooLong(
                raw_request[0..1000].as_bytes().to_vec()
            )),
            request.parse(raw_request)
        );
    }

    #[test]
    fn max_message_size_checked_for_headers() {
        let mut request = Request::new();
        request.max_message_size = Some(150);
        let small_request = concat!(
            "GET /hello.txt HTTP/1.1\r\n",
            "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n",
            "Host: www.example.com\r\n",
            "Accept-Language: en, mi\r\n",
            "\r\n",
        );
        assert_eq!(
            Ok((ParseStatus::Complete, small_request.len())),
            request.parse(small_request)
        );
        request = Request::new();
        request.max_message_size = Some(150);
        let large_request = concat!(
            "GET /hello.txt HTTP/1.1\r\n",
            "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n",
            "Host: www.example.com\r\n",
            "Accept-Language: en, mi\r\n",
            "X-PogChamp-Level: Over 9000\r\n",
            "\r\n",
        );
        assert_eq!(
            Err(Error::MessageTooLong),
            request.parse(large_request)
        );
    }

    #[test]
    fn max_message_size_checked_for_total() {
        let mut request = Request::new();
        request.max_message_size = Some(125);
        let small_request = concat!(
            "POST / HTTP/1.1\r\n",
            "Host: foo.com\r\n",
            "Content-Type: application/x-www-form-urlencoded\r\n",
            "Content-Length: 15\r\n",
            "\r\n",
            "say=Hi&to=Mom\r\n",
        );
        assert_eq!(
            Ok((ParseStatus::Complete, small_request.len())),
            request.parse(small_request)
        );
        request = Request::new();
        request.max_message_size = Some(125);
        let large_request = concat!(
            "POST / HTTP/1.1\r\n",
            "Host: foo.com\r\n",
            "Content-Type: application/x-www-form-urlencoded\r\n",
            "Content-Length: 102\r\n",
            "\r\n",
            "say=Hi&to=Mom&listen_to=lecture&content=remember_to_brush_your_teeth_and_always_wear_clean_underwear\r\n",
        );
        assert_eq!(
            Err(Error::MessageTooLong),
            request.parse(large_request)
        );
    }

}
