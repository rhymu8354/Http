/**
 * @file ChunkedBody.cpp
 *
 * This module contains the implementation of the Http::ChunkedBody class.
 *
 * © 2018 by Richard Walters
 */

#include <algorithm>
#include <Http/ChunkedBody.hpp>
#include <limits>
#include <sstream>
#include <stddef.h>
#include <string>

namespace {

    /**
     * This is the required line terminator for encoded chunk body lines.
     */
    const std::string CRLF = "\r\n";

    /**
     * These are the characters which are valid for use in tokens.
     */
    const std::string TCHAR = "!#$%&'*+-.^_`|~0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

    /**
     * This is the mode flag combination to use when initializing
     * output string streams in this module.
     */
    constexpr std::ios_base::openmode OUTPUT_STRING_STREAM_OPEN_MODE = (
        std::ios_base::out          // output, of course
        | std::ios_base::binary     // binary; don't confuse us with LF -> CRLF
        | std::ios_base::ate        // seek to the end always when opening
    );

    /**
     * This function decodes the given line of input as a chunk-size line.
     *
     * Any chunk extensions are parsed but discarded.
     *
     * @param[in] input
     *     This is the string holding the line of input.
     *
     * @param[in] lineLength
     *     This is the length of the line of input.
     *
     * @param[out] chunkSize
     *     This is where to store the decoded chunk-size value.
     *
     * @return
     *     An indication of whether or not the chunk-size line was
     *     successfully parsed is returned.
     */
    bool DecodeChunkSizeLine(
        const std::string& input,
        size_t lineLength,
        size_t& chunkSize
    ) {
        int state = 0;
        chunkSize = 0;
        for (size_t offset = 0; offset < lineLength; ++offset) {
            const auto c = input[offset];
            switch (state) {
                case 0: { // chunk-size
                    size_t nextDigit = 0;
                    if ((c >= '0') && (c <= '9')) {
                        nextDigit = (size_t)(c - '0');
                    } else if ((c >= 'A') && (c <= 'F')) {
                        nextDigit = (size_t)(c - 'A') + 10;
                    } else if ((c >= 'a') && (c <= 'f')) {
                        nextDigit = (size_t)(c - 'a') + 10;
                    } else if (c == ';') {
                        state = 1;
                        continue;
                    } else {
                        return false;
                    }
                    if ((std::numeric_limits< size_t >::max() - nextDigit) / 16 < chunkSize) {
                        return false;
                    }
                    chunkSize <<= 4;
                    chunkSize += nextDigit;
                } break;

                case 1: { // chunk-ext: chunk-ext-name (first character)
                    if (TCHAR.find(c) == std::string::npos) {
                        return false;
                    }
                    state = 2;
                } break;

                case 2: { // chunk-ext: chunk-ext-name (not first character)
                    if (c == '=') {
                        state = 3;
                    } else if (c == ';') {
                        state = 1;
                    } else if (TCHAR.find(c) == std::string::npos) {
                        return false;
                    }
                } break;

                case 3: { // chunk-ext: chunk-ext-val (first character)
                    if (c == '"') {
                        state = 5;
                    } else if (TCHAR.find(c) == std::string::npos) {
                        return false;
                    } else {
                        state = 4;
                    }
                } break;

                case 4: { // chunk-ext: chunk-ext-val (token, not first character)
                    if (c == ';') {
                        state = 1;
                    } else if (TCHAR.find(c) == std::string::npos) {
                        return false;
                    }
                } break;

                case 5: { // chunk-ext: chunk-ext-val (quoted-string, not first character)
                    if (c == '"') {
                        state = 7;
                    } else if (c == '\\') {
                        state = 6;
                    } else if (
                        (c != '\t')
                        && (c != ' ')
                        && (c != '!')
                        && ((c < 0x23) || (c > 0x5B))
                        && ((c < '#') || (c > '['))
                        && ((c < 0x5D) || (c > 0x7E))
                        && ((c < ']') || (c > '~'))
                        && (c > 0)
                    ) {
                        return false;
                    }
                } break;

                case 6: { // chunk-ext: chunk-ext-val (quoted-string, second character of quoted-pair)
                    if (
                        (c != '\t')
                        && (c != ' ')
                        && ((c < ' ') || (c > 0x7E))
                    ) {
                        return false;
                    }
                    state = 5;
                } break;

                case 7: { // chunk-ext: chunk-ext (next character after last extension)
                    if (c == ';') {
                        state = 1;
                    } else {
                        return false;
                    }
                } break;
            }
        }
        return (
            (state == 0)
            || (state == 2)
            || (state == 4)
            || (state == 7)
        );
    }

}

namespace Http {

    /**
     * This contains the private properties of a ChunkedBody instance.
     */
    struct ChunkedBody::Impl {
        // Properties

        /**
         * This is the current state of the decoding of the chunked body.
         */
        State state = State::DecodingChunks;

        /**
         * If we're in the ReadingChunkData state, this is the number of
         * bytes that still need to be input.
         */
        size_t currentChunkBytesMissing = 0;

        /**
         * This contains the actual decoded body.
         */
        std::ostringstream decodedBody;

        /**
         * This is used to reassemble the encoded chunk before decoding it.
         */
        std::ostringstream reassemblyBuffer;

        // Methods

        /**
         * This is the default constructor.
         */
        Impl()
            : decodedBody(OUTPUT_STRING_STREAM_OPEN_MODE)
            , reassemblyBuffer(OUTPUT_STRING_STREAM_OPEN_MODE)
        {
        }
    };

    ChunkedBody::~ChunkedBody() = default;

    ChunkedBody::ChunkedBody()
        : impl_(new Impl)
    {
    }

    size_t ChunkedBody::Decode(
        const std::string& input,
        size_t offset,
        size_t length
    ) {
        const size_t charactersPreviouslyAccepted = impl_->reassemblyBuffer.tellp();
        if (length == 0) {
            length = input.length();
        }
        impl_->reassemblyBuffer << input.substr(offset, length);
        size_t charactersAccepted = 0;
        while (
            (impl_->reassemblyBuffer.tellp() > 0)
            && (impl_->state != State::Complete)
            && (impl_->state != State::Error)
        ) {
            if (impl_->state == State::DecodingChunks) {
                const auto reassembledInput = impl_->reassemblyBuffer.str();
                const auto lineEnd = reassembledInput.find(CRLF);
                if (lineEnd == std::string::npos) {
                    charactersAccepted += impl_->reassemblyBuffer.tellp();
                    break;
                }
                const auto lineLength = lineEnd + CRLF.length();
                charactersAccepted += lineLength;
                if (
                    !DecodeChunkSizeLine(
                        reassembledInput,
                        lineEnd,
                        impl_->currentChunkBytesMissing
                    )
                ) {
                    impl_->state = State::Error;
                    break;
                }
                impl_->reassemblyBuffer = std::ostringstream(reassembledInput.substr(lineLength), OUTPUT_STRING_STREAM_OPEN_MODE);
                if (impl_->currentChunkBytesMissing == 0) {
                    impl_->state = State::DecodingTrailer;
                } else {
                    impl_->state = State::ReadingChunkData;
                }
            }
            if (impl_->state == State::ReadingChunkData) {
                const size_t chunkDataAvailableFromInput = impl_->reassemblyBuffer.tellp();
                const auto chunkDataToCopyFromInput = std::min(
                    chunkDataAvailableFromInput,
                    impl_->currentChunkBytesMissing
                );
                if (chunkDataToCopyFromInput > 0) {
                    const auto reassembledInput = impl_->reassemblyBuffer.str();
                    if (reassembledInput.length() == chunkDataToCopyFromInput) {
                        impl_->decodedBody << reassembledInput;
                        impl_->reassemblyBuffer.clear();
                    } else {
                        impl_->decodedBody << reassembledInput.substr(0, chunkDataToCopyFromInput);
                        impl_->reassemblyBuffer = std::ostringstream(reassembledInput.substr(chunkDataToCopyFromInput), OUTPUT_STRING_STREAM_OPEN_MODE);
                    }
                    charactersAccepted += chunkDataToCopyFromInput;
                    impl_->currentChunkBytesMissing -= chunkDataToCopyFromInput;
                    if (impl_->currentChunkBytesMissing == 0) {
                        impl_->state = State::ReadingChunkDelimiter;
                    }
                }
            }
            if (impl_->state == State::ReadingChunkDelimiter) {
                if ((size_t)impl_->reassemblyBuffer.tellp() < CRLF.length()) {
                    charactersAccepted += impl_->reassemblyBuffer.tellp();
                    break;
                }
                const auto reassembledInput = impl_->reassemblyBuffer.str();
                if (reassembledInput.substr(0, CRLF.length()) != CRLF) {
                    impl_->state = State::Error;
                    break;
                }
                charactersAccepted += CRLF.length();
                impl_->reassemblyBuffer = std::ostringstream(reassembledInput.substr(CRLF.length()), OUTPUT_STRING_STREAM_OPEN_MODE);
                impl_->state = State::DecodingTrailer;
            }
            if (impl_->state == State::DecodingTrailer) {
                // TODO: Actually decode trailers.  For now, we're assuming no trailers.
                if ((size_t)impl_->reassemblyBuffer.tellp() < CRLF.length()) {
                    charactersAccepted += impl_->reassemblyBuffer.tellp();
                    break;
                }
                const auto reassembledInput = impl_->reassemblyBuffer.str();
                if (reassembledInput.substr(0, CRLF.length()) != CRLF) {
                    impl_->state = State::Error;
                    break;
                }
                charactersAccepted += CRLF.length();
                impl_->reassemblyBuffer = std::ostringstream(reassembledInput.substr(CRLF.length()), OUTPUT_STRING_STREAM_OPEN_MODE);
                impl_->state = State::Complete;
            }
        }
        return charactersAccepted - charactersPreviouslyAccepted;
    }

    auto ChunkedBody::GetState() const -> State {
        return impl_->state;
    }

    ChunkedBody::operator std::string() const {
        return impl_->decodedBody.str();
    }

    void PrintTo(
        const Http::ChunkedBody::State& state,
        std::ostream* os
    ) {
        switch (state) {
            case Http::ChunkedBody::State::DecodingChunks: {
                *os << "Decoding chunks";
            } break;
            case Http::ChunkedBody::State::ReadingChunkData: {
                *os << "Reading chunk data";
            } break;
            case Http::ChunkedBody::State::ReadingChunkDelimiter: {
                *os << "Reading chunk delimiter";
            } break;
            case Http::ChunkedBody::State::DecodingTrailer: {
                *os << "Decoding trailer";
            } break;
            case Http::ChunkedBody::State::Complete: {
                *os << "COMPLETE";
            } break;
            case Http::ChunkedBody::State::Error: {
                *os << "ERROR";
            } break;
            default: {
                *os << "???";
            };
        }
    }

}
