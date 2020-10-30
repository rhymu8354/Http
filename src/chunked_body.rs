use super::{
    error::Error,
    find_crlf,
    CRLF,
};
use rhymessage::MessageHeaders;

fn parse_chunk_size(chunk_size_line: &str) -> Result<usize, Error> {
    let delimiter = chunk_size_line
        .find(|c| c == ';' || c == '\r')
        .unwrap_or_else(|| chunk_size_line.len());
    let chunk_size = &chunk_size_line[..delimiter];
    usize::from_str_radix(chunk_size, 16).map_err(Error::InvalidChunkSize)
}

#[derive(Debug, Eq, PartialEq)]
pub enum DecodeStatus {
    Complete,
    Incomplete,
}

enum DecodeStatusInternal {
    CompletePart,
    CompleteWhole,
    Incomplete,
}

#[derive(Debug, Eq, PartialEq)]
enum ChunkedBodyState {
    ChunkData,
    ChunkSize,
    ChunkTerminator,
    Trailer,
}

pub struct ChunkedBody {
    pub buffer: Vec<u8>,
    chunk_bytes_needed: usize,
    state: ChunkedBodyState,
    pub trailer: MessageHeaders,
}

impl ChunkedBody {
    #[cfg(test)]
    fn as_bytes(&self) -> &[u8] {
        &self.buffer
    }

    pub fn decode<T>(
        &mut self,
        input: T,
    ) -> Result<(DecodeStatus, usize), Error>
    where
        T: AsRef<[u8]>,
    {
        let input = input.as_ref();
        let mut total_consumed = 0;
        loop {
            let input_remainder = &input[total_consumed..];
            let (decode_status, consumed) = match self.state {
                ChunkedBodyState::ChunkData => {
                    self.decode_data(input_remainder)
                },
                ChunkedBodyState::ChunkSize => {
                    self.decode_size(input_remainder)?
                },
                ChunkedBodyState::ChunkTerminator => {
                    self.decode_terminator(input_remainder)?
                },
                ChunkedBodyState::Trailer => {
                    self.decode_trailer(input_remainder)?
                },
            };
            total_consumed += consumed;
            match decode_status {
                DecodeStatusInternal::CompletePart => (),
                DecodeStatusInternal::CompleteWhole => {
                    return Ok((DecodeStatus::Complete, total_consumed));
                },
                DecodeStatusInternal::Incomplete => {
                    return Ok((DecodeStatus::Incomplete, total_consumed));
                },
            };
        }
    }

    fn decode_data(
        &mut self,
        raw_message: &[u8],
    ) -> (DecodeStatusInternal, usize) {
        let consumed = raw_message.len().min(self.chunk_bytes_needed);
        self.chunk_bytes_needed -= consumed;
        self.buffer.extend(&raw_message[..consumed]);
        if self.chunk_bytes_needed == 0 {
            self.state = ChunkedBodyState::ChunkTerminator;
            (DecodeStatusInternal::CompletePart, consumed)
        } else {
            (DecodeStatusInternal::Incomplete, consumed)
        }
    }

    fn decode_size(
        &mut self,
        raw_message: &[u8],
    ) -> Result<(DecodeStatusInternal, usize), Error> {
        match find_crlf(raw_message) {
            Some(chunk_size_line_end) => {
                let chunk_size_line = &raw_message[0..chunk_size_line_end];
                let chunk_size_line = std::str::from_utf8(chunk_size_line)
                    .map_err(|_| {
                        Error::ChunkSizeLineNotValidText(
                            chunk_size_line.to_vec(),
                        )
                    })?;
                let consumed = chunk_size_line_end + CRLF.len();
                self.chunk_bytes_needed = parse_chunk_size(chunk_size_line)?;
                self.buffer
                    .reserve(self.buffer.len() + self.chunk_bytes_needed);
                self.state = match self.chunk_bytes_needed {
                    0 => ChunkedBodyState::Trailer,
                    _ => ChunkedBodyState::ChunkData,
                };
                Ok((DecodeStatusInternal::CompletePart, consumed))
            },
            None => Ok((DecodeStatusInternal::Incomplete, 0)),
        }
    }

    fn decode_terminator(
        &mut self,
        raw_message: &[u8],
    ) -> Result<(DecodeStatusInternal, usize), Error> {
        match raw_message {
            [] | [b'\r'] => Ok((DecodeStatusInternal::Incomplete, 0)),
            [b'\r', b'\n', ..] => {
                self.state = ChunkedBodyState::ChunkSize;
                Ok((DecodeStatusInternal::CompletePart, 2))
            },
            _ => Err(Error::InvalidChunkTerminator(raw_message.to_vec())),
        }
    }

    fn decode_trailer(
        &mut self,
        raw_message: &[u8],
    ) -> Result<(DecodeStatusInternal, usize), Error> {
        let parse_results =
            self.trailer.parse(raw_message).map_err(Error::Trailer)?;
        match parse_results.status {
            rhymessage::ParseStatus::Complete => Ok((
                DecodeStatusInternal::CompleteWhole,
                parse_results.consumed,
            )),
            rhymessage::ParseStatus::Incomplete => {
                Ok((DecodeStatusInternal::Incomplete, parse_results.consumed))
            },
        }
    }

    pub fn new() -> Self {
        Self {
            buffer: Vec::new(),
            chunk_bytes_needed: 0,
            state: ChunkedBodyState::ChunkSize,
            trailer: MessageHeaders::new(),
        }
    }
}

#[cfg(test)]
mod tests {

    use super::*;

    #[test]
    fn decode_simple_empty_body_one_piece() {
        let input = "0\r\n\r\n";
        let mut body = ChunkedBody::new();
        assert!(matches!(
            body.decode(input),
            Ok((DecodeStatus::Complete, consumed)) if consumed == input.len()
        ));
        assert_eq!(b"", body.as_bytes());
    }

    #[test]
    fn decode_empty_body_multiple_zeroes() {
        let input = "00000\r\n\r\n";
        let mut body = ChunkedBody::new();
        assert!(matches!(
            body.decode(input),
            Ok((DecodeStatus::Complete, consumed)) if consumed == input.len()
        ));
        assert_eq!(b"", body.as_bytes());
    }

    #[test]
    fn decode_empty_body_with_chunk_extension_no_value() {
        let input = "000;dude\r\n\r\n";
        let mut body = ChunkedBody::new();
        assert!(matches!(
            body.decode(input),
            Ok((DecodeStatus::Complete, consumed)) if consumed == input.len()
        ));
        assert_eq!(b"", body.as_bytes());
    }

    #[test]
    fn decode_empty_body_with_chunk_extension_with_unquoted_value() {
        let input = "000;Kappa=PogChamp\r\n\r\n";
        let mut body = ChunkedBody::new();
        assert!(matches!(
            body.decode(input),
            Ok((DecodeStatus::Complete, consumed)) if consumed == input.len()
        ));
        assert_eq!(b"", body.as_bytes());
    }

    #[test]
    fn decode_empty_body_with_chunk_extension_with_quoted_value() {
        let input = "000;Kappa=\"Hello, World!\"\r\n\r\n";
        let mut body = ChunkedBody::new();
        assert!(matches!(
            body.decode(input),
            Ok((DecodeStatus::Complete, consumed)) if consumed == input.len()
        ));
        assert_eq!(b"", body.as_bytes());
    }

    #[test]
    fn decode_empty_body_with_multiple_chunk_extensions() {
        let input = "000;Foo=Bar;Kappa=\"Hello, World!\";Spam=12345!\r\n\r\n";
        let mut body = ChunkedBody::new();
        assert!(matches!(
            body.decode(input),
            Ok((DecodeStatus::Complete, consumed)) if consumed == input.len()
        ));
        assert_eq!(b"", body.as_bytes());
    }

    #[test]
    fn decode_simple_empty_body_one_character_at_a_time() {
        let input = b"0\r\n\r\n";
        let mut accepted = 0;
        let mut body = ChunkedBody::new();
        for i in 0..input.len() {
            let (status, consumed) = body.decode(&input[accepted..=i]).unwrap();
            accepted += consumed;
            match i {
                0..=1 => {
                    assert_eq!(DecodeStatus::Incomplete, status, "{}", i);
                    assert_eq!(
                        ChunkedBodyState::ChunkSize,
                        body.state,
                        "{}",
                        i
                    );
                    assert_eq!(0, accepted);
                },
                2..=3 => {
                    assert_eq!(DecodeStatus::Incomplete, status, "{}", i);
                    assert_eq!(ChunkedBodyState::Trailer, body.state, "{}", i);
                    assert_eq!(3, accepted);
                },
                _ => {
                    assert_eq!(DecodeStatus::Complete, status, "{}", i);
                    assert_eq!(5, accepted);
                },
            }
        }
        assert_eq!(b"", body.as_bytes());
    }

    #[test]
    fn decode_simple_empty_body_one_piece_with_extra_stuff_after() {
        let input = "0\r\n\r\nHello!";
        let mut body = ChunkedBody::new();
        assert!(matches!(body.decode(input), Ok((DecodeStatus::Complete, 5))));
        assert_eq!(b"", body.as_bytes());
    }

    #[test]
    fn decode_simple_empty_body_two_pieces() {
        let input = "XYZ0\r\n\r\n123";
        let mut body = ChunkedBody::new();
        assert!(matches!(
            body.decode(&input[3..7]),
            Ok((DecodeStatus::Incomplete, 3))
        ));
        assert_eq!(ChunkedBodyState::Trailer, body.state);
        assert!(matches!(
            body.decode(&input[6..9]),
            Ok((DecodeStatus::Complete, 2))
        ));
        assert_eq!(b"", body.as_bytes());
    }

    #[test]
    fn decode_simple_non_empty_body_one_piece() {
        let input = "5\r\nHello\r\n0\r\n\r\n";
        let mut body = ChunkedBody::new();
        assert!(matches!(
            body.decode(input),
            Ok((DecodeStatus::Complete, consumed)) if consumed == input.len()
        ));
        assert_eq!(b"Hello", body.as_bytes());
    }

    #[test]
    fn decode_simple_non_empty_body_one_character_at_a_time() {
        let input = "5\r\nHello\r\n0\r\n\r\n";
        let mut accepted = 0;
        let mut body = ChunkedBody::new();
        for i in 0..input.len() {
            let (status, consumed) = body.decode(&input[accepted..=i]).unwrap();
            accepted += consumed;
            match i {
                0..=1 => {
                    assert_eq!(DecodeStatus::Incomplete, status, "{}", i);
                    assert_eq!(
                        ChunkedBodyState::ChunkSize,
                        body.state,
                        "{}",
                        i
                    );
                    assert_eq!(0, accepted);
                },
                2..=6 => {
                    assert_eq!(DecodeStatus::Incomplete, status, "{}", i);
                    assert_eq!(
                        ChunkedBodyState::ChunkData,
                        body.state,
                        "{}",
                        i
                    );
                    assert_eq!(i + 1, accepted);
                },
                7..=8 => {
                    assert_eq!(DecodeStatus::Incomplete, status, "{}", i);
                    assert_eq!(
                        ChunkedBodyState::ChunkTerminator,
                        body.state,
                        "{}",
                        i
                    );
                    assert_eq!(8, accepted);
                },
                9..=11 => {
                    assert_eq!(DecodeStatus::Incomplete, status, "{}", i);
                    assert_eq!(
                        ChunkedBodyState::ChunkSize,
                        body.state,
                        "{}",
                        i
                    );
                    assert_eq!(10, accepted);
                },
                12..=13 => {
                    assert_eq!(DecodeStatus::Incomplete, status, "{}", i);
                    assert_eq!(ChunkedBodyState::Trailer, body.state, "{}", i);
                    assert_eq!(13, accepted);
                },
                _ => {
                    assert_eq!(DecodeStatus::Complete, status, "{}", i);
                    assert_eq!(15, accepted);
                },
            }
        }
        assert_eq!(b"Hello", body.as_bytes());
    }

    #[test]
    fn decode_two_chunk_body_one_piece() {
        let input = "6\r\nHello,\r\n7\r\n World!\r\n0\r\n\r\n";
        let mut body = ChunkedBody::new();
        assert!(matches!(
            body.decode(input),
            Ok((DecodeStatus::Complete, consumed)) if consumed == input.len()
        ));
        assert_eq!(b"Hello, World!", body.as_bytes());
    }

    #[test]
    fn decode_two_chunk_body_one_character_at_a_time() {
        let input = "6\r\nHello,\r\n7\r\n World!\r\n0\r\n\r\n";
        let mut accepted = 0;
        let mut body = ChunkedBody::new();
        for i in 0..input.len() {
            let (status, consumed) = body.decode(&input[accepted..=i]).unwrap();
            accepted += consumed;
            // This warning would have us collapse two arms together, but
            // I think it makes it easier to understand the decoding process
            // if the arms are left in chronological order.
            #[allow(clippy::match_same_arms)]
            match i {
                0..=1 => {
                    assert_eq!(DecodeStatus::Incomplete, status, "{}", i);
                    assert_eq!(
                        ChunkedBodyState::ChunkSize,
                        body.state,
                        "{}",
                        i
                    );
                    assert_eq!(0, accepted);
                },
                2..=7 => {
                    assert_eq!(DecodeStatus::Incomplete, status, "{}", i);
                    assert_eq!(
                        ChunkedBodyState::ChunkData,
                        body.state,
                        "{}",
                        i
                    );
                    assert_eq!(i + 1, accepted);
                },
                8..=9 => {
                    assert_eq!(DecodeStatus::Incomplete, status, "{}", i);
                    assert_eq!(
                        ChunkedBodyState::ChunkTerminator,
                        body.state,
                        "{}",
                        i
                    );
                    assert_eq!(9, accepted);
                },
                10..=12 => {
                    assert_eq!(DecodeStatus::Incomplete, status, "{}", i);
                    assert_eq!(
                        ChunkedBodyState::ChunkSize,
                        body.state,
                        "{}",
                        i
                    );
                    assert_eq!(11, accepted);
                },
                13..=19 => {
                    assert_eq!(DecodeStatus::Incomplete, status, "{}", i);
                    assert_eq!(
                        ChunkedBodyState::ChunkData,
                        body.state,
                        "{}",
                        i
                    );
                    assert_eq!(i + 1, accepted);
                },
                20..=21 => {
                    assert_eq!(DecodeStatus::Incomplete, status, "{}", i);
                    assert_eq!(
                        ChunkedBodyState::ChunkTerminator,
                        body.state,
                        "{}",
                        i
                    );
                    assert_eq!(21, accepted);
                },
                22..=24 => {
                    assert_eq!(DecodeStatus::Incomplete, status, "{}", i);
                    assert_eq!(
                        ChunkedBodyState::ChunkSize,
                        body.state,
                        "{}",
                        i
                    );
                    assert_eq!(23, accepted);
                },
                25..=26 => {
                    assert_eq!(DecodeStatus::Incomplete, status, "{}", i);
                    assert_eq!(ChunkedBodyState::Trailer, body.state, "{}", i);
                    assert_eq!(26, accepted);
                },
                _ => {
                    assert_eq!(DecodeStatus::Complete, status, "{}", i);
                    assert_eq!(28, accepted);
                },
            }
        }
        assert_eq!(b"Hello, World!", body.as_bytes());
    }

    #[test]
    fn decode_trailers_one_piece() {
        let input = "0\r\nX-Foo: Bar\r\nX-Poggers: FeelsBadMan\r\n\r\n";
        let mut body = ChunkedBody::new();
        assert!(matches!(
            body.decode(input),
            Ok((DecodeStatus::Complete, consumed)) if consumed == input.len()
        ));
        assert_eq!(b"", body.as_bytes());
        assert_eq!(
            &vec![
                rhymessage::Header {
                    name: "X-Foo".into(),
                    value: "Bar".into(),
                },
                rhymessage::Header {
                    name: "X-Poggers".into(),
                    value: "FeelsBadMan".into(),
                },
            ],
            body.trailer.headers()
        );
    }

    #[test]
    fn decode_trailers_one_character_at_a_time() {
        let input = "0\r\nX-Foo: Bar\r\nX-Poggers: FeelsBadMan\r\n\r\n";
        let mut accepted = 0;
        let mut body = ChunkedBody::new();
        for i in 0..input.len() {
            let (status, consumed) = body.decode(&input[accepted..=i]).unwrap();
            accepted += consumed;
            // This warning would have us collapse two arms together, but
            // I think it makes it easier to understand the decoding process
            // if the arms are left in chronological order.
            #[allow(clippy::match_same_arms)]
            match i {
                0..=1 => {
                    assert_eq!(DecodeStatus::Incomplete, status, "{}", i);
                    assert_eq!(
                        ChunkedBodyState::ChunkSize,
                        body.state,
                        "{}",
                        i
                    );
                    assert_eq!(0, accepted);
                },
                2..=37 => {
                    assert_eq!(DecodeStatus::Incomplete, status, "{}", i);
                    assert_eq!(ChunkedBodyState::Trailer, body.state, "{}", i);
                    assert_eq!(3, accepted);
                },
                38..=39 => {
                    assert_eq!(DecodeStatus::Incomplete, status, "{}", i);
                    assert_eq!(ChunkedBodyState::Trailer, body.state, "{}", i);
                    assert_eq!(15, accepted);
                },
                _ => {
                    assert_eq!(DecodeStatus::Complete, status, "{}", i);
                    assert_eq!(41, accepted);
                },
            }
        }
        assert_eq!(b"", body.as_bytes());
        assert_eq!(
            &vec![
                rhymessage::Header {
                    name: "X-Foo".into(),
                    value: "Bar".into(),
                },
                rhymessage::Header {
                    name: "X-Poggers".into(),
                    value: "FeelsBadMan".into(),
                },
            ],
            body.trailer.headers()
        );
    }

    #[test]
    fn decode_bad_chunk_size_line_not_hexdig_in_chunk_size() {
        let input = "0g\r\n\r\n";
        let mut body = ChunkedBody::new();
        assert!(matches!(body.decode(input), Err(Error::InvalidChunkSize(_))));
    }

    #[test]
    fn decode_bad_chunk_size_line_chunk_size_overflow() {
        let input = "111111111111111111111111111111111111111111111111111111111111111\r\n\r\n";
        let mut body = ChunkedBody::new();
        assert!(matches!(
            dbg!(body.decode(input)),
            Err(Error::InvalidChunkSize(_))
        ));
    }

    #[test]
    fn decode_bad_junk_after_chunk() {
        let input = "1\r\nXjunk\r\n";
        let mut body = ChunkedBody::new();
        assert!(matches!(
            body.decode(input),
            Err(Error::InvalidChunkTerminator(junk)) if junk == b"junk\r\n"
        ));
    }

    #[test]
    fn decode_bad_trailer() {
        let input = "0\r\nX-Foo Bar\r\n\r\n";
        let mut body = ChunkedBody::new();
        assert!(matches!(
            body.decode(input),
            Err(Error::Trailer(
                rhymessage::Error::HeaderLineMissingColon(line)
            )) if line == "X-Foo Bar"
        ));
    }
}
