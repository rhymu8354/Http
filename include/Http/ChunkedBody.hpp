#ifndef HTTP_CHUNKED_BODY_HPP
#define HTTP_CHUNKED_BODY_HPP

/**
 * @file ChunkedBody.hpp
 *
 * This module declares the Http::ChunkedBody class.
 *
 * © 2018 by Richard Walters
 */

#include <MessageHeaders/MessageHeaders.hpp>
#include <memory>
#include <string>

namespace Http {

    /**
     * This is part of the Http library, which implements
     * [RFC 7230](https://tools.ietf.org/html/rfc7230),
     * "Hypertext Transfer Protocol (HTTP/1.1): Message Syntax and Routing".
     *
     * This class is used to decode the body of a request or response
     * which is using Chunked Transfer Encoding, as described by
     * section 4.1 of the RFC.
     */
    class ChunkedBody {
        // Types
    public:
        /**
         * These are the different states that the chunked body can have.
         */
        enum class State {
            /**
             * End of chunks not yet found, decoding next chunk-size line.
             */
            DecodingChunks,

            /**
             * End of chunks not yet found, reading next chunk data.
             */
            ReadingChunkData,

            /**
             * End of chunks not yet found, reading delimiter of last chunk.
             */
            ReadingChunkDelimiter,

            /**
             * End of trailer not yet found.
             */
            DecodingTrailer,

            /**
             * End of chunked body and trailer found.
             */
            Complete,

            /**
             * Unrecoverable error; reject input.
             */
            Error,
        };

        // Lifecycle management
    public:
        ~ChunkedBody() noexcept;
        ChunkedBody(const ChunkedBody&) = delete;
        ChunkedBody(ChunkedBody&&) noexcept = delete;
        ChunkedBody& operator=(const ChunkedBody&) = delete;
        ChunkedBody& operator=(ChunkedBody&&) noexcept = delete;

        // Public methods
    public:
        /**
         * This is the default constructor.
         */
        ChunkedBody();

        /**
         * This method continues the decoding of the chunked body,
         * passing more characters into the decoding process.
         *
         * @note
         *     Call GetState afterwards to determine whether or not
         *     the decoding process encountered an error or if the body
         *     decoding is complete.
         *
         * @param[in] input
         *     This contains the characters to input into the
         *     decoding process.
         *
         * @param[in] offset
         *     This is the position of the first character in the given
         *     input string which should be input into the decoding process.
         *
         * @param[in] length
         *     This is the number of characters to input into the
         *     decoding process.  If zero, all characters in the given string
         *     from the offset to the end of the string are input.
         *
         * @return
         *     The number of characters accepted into the decoding process
         *     is returned.
         */
        size_t Decode(
            const std::string& input,
            size_t offset = 0,
            size_t length = 0
        );

        /**
         * This method returns the current state of the decoding process.
         *
         * @return
         *     The current state of the decoding process is returned.
         */
        State GetState() const;

        /**
         * This method returns the body as a string.
         *
         * @return
         *     The body as a string is returned.
         */
        operator std::string() const;

        /**
         * This method returns an object holding any trailers
         * that were attached to the chunked body.
         *
         * @return
         *     An object holding any trailers that were attached
         *     to the chunked body is returned.
         */
        const MessageHeaders::MessageHeaders& GetTrailers() const;

        // Private properties
    private:
        /**
         * This is the type of structure that contains the private
         * properties of the instance.  It is defined in the implementation
         * and declared here to ensure that it is scoped inside the class.
         */
        struct Impl;

        /**
         * This contains the private properties of the instance.
         */
        std::unique_ptr< Impl > impl_;
    };

    /**
     * This is a support function for Google Test to print out
     * values of the Http::ChunkedBody::State class.
     *
     * @param[in] state
     *     This is the HTTP chunked body decoding state value to print.
     *
     * @param[in] os
     *     This points to the stream to which to print the
     *     HTTP chunked body decoding state value.
     */
    void PrintTo(
        const Http::ChunkedBody::State& state,
        std::ostream* os
    );

}

#endif /* HTTP_CHUNKED_BODY_HPP */
