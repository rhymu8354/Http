use rhymessage::MessageHeaders;
use std::io::Write;
use super::error::Error;

enum ResponseState {
    Body,
    Complete,
    Error,
    Headers,
    StatusLine,
}

pub struct Response {
    body: Vec<u8>,
    headers: MessageHeaders,
    reason_phrase: std::borrow::Cow<'static, str>,
    state: ResponseState,
    status_code: usize,
}

impl Response {
    pub fn generate(&self) -> Result<Vec<u8>, Error> {
        let mut output = Vec::new();
        write!(&mut output, "HTTP/1.1 {} {}\r\n", self.status_code, self.reason_phrase)
            .map_err(|_| Error::StringFormat)?;
        output.append(&mut self.headers.generate()?);
        output.extend(&self.body);
        Ok(output)
    }

    // TODO: Possibly remove this if `parse` returns this information instead.
    fn is_complete_or_error(&self, more_data_possible: bool) -> bool {
        match self.state {
            ResponseState::Complete | ResponseState::Error => true,
            ResponseState::Body => {
                !more_data_possible
                && !self.headers.has_header("Content-Length")
                && !self.headers.has_header_token("Transfer-Encoding", "chunked")
            }
            _ => false,
        }
    }

    #[must_use]
    pub fn new() -> Self {
        Self{
            body: Vec::new(),
            headers: MessageHeaders::new(),
            reason_phrase: "OK".into(),
            state: ResponseState::StatusLine,
            status_code: 200,
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
    fn is_complete_or_error() {
        let mut response = Response::new();
        response.state = ResponseState::Complete;
        assert!(response.is_complete_or_error(true));
        response.state = ResponseState::Error;
        assert!(response.is_complete_or_error(true));
        response.state = ResponseState::Headers;
        assert!(!response.is_complete_or_error(true));
        response.state = ResponseState::StatusLine;
        assert!(!response.is_complete_or_error(true));
        response.state = ResponseState::Body;
        assert!(!response.is_complete_or_error(true));
        assert!(response.is_complete_or_error(false));
        response = Response::new();
        response.headers.set_header("Content-Length", "42");
        assert!(!response.is_complete_or_error(true));
        assert!(!response.is_complete_or_error(false));
        response = Response::new();
        response.headers.set_header("Transfer-Encoding", "chunked");
        assert!(!response.is_complete_or_error(true));
        assert!(!response.is_complete_or_error(false));
    }

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

}
