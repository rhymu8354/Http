#ifndef HTTP_RESPONSE_HPP
#define HTTP_RESPONSE_HPP

/**
 * @file Response.hpp
 *
 * This module declares the Http::Response structure.
 *
 * © 2018 by Richard Walters
 */

#include <MessageHeaders/MessageHeaders.hpp>
#include <string>

namespace Http {

    /**
     * This represents an overall HTTP response given to a client,
     * decomposed into its various elements.
     */
    struct Response {
        // Types

        /**
         * This type is used to track how much of the response
         * has been constructed so far.
         */
        enum class State {
            /**
             * In this state, we're still waiting to construct
             * the full status line.
             */
            StatusLine,

            /**
             * In this state, we've constructed the status
             * line, and possibly some header lines, but haven't yet
             * constructed all of the header lines.
             */
            Headers,

            /**
             * In this state, we've constructed the status
             * line and headers, and possibly some of the body, but
             * haven't yet constructed all of the body.
             */
            Body,

            /**
             * In this state, the response either fully constructed
             * or is invalid, but the connection from which the response
             * was constructed can remain open to accept another response.
             */
            Complete,

            /**
             * In this state, the connection from which the response
             * was constructed should be closed, either for security
             * reasons, or because it would be impossible
             * or unlikely to receive a valid response after
             * this one.
             */
            Error,
        };

        // Properties

        /**
         * This flag indicates whether or not the request
         * has passed all validity checks.
         */
        bool valid = true;

        /**
         * This is a machine-readable number that describes
         * the overall status of the request.
         */
        unsigned int statusCode;

        /**
         * This is the human-readable text that describes
         * the overall status of the request.
         */
        std::string reasonPhrase;

        /**
         * These are the message headers that were included
         * in the request.
         */
        MessageHeaders::MessageHeaders headers;

        /**
         * This is the body of the request, if there is a body.
         */
        std::string body;

        /**
         * This indicates whether or not the response
         * passed all validity checks when it was parsed,
         * and if not, whether or not the connection can
         * still be used.
         */
        State state = State::StatusLine;

        // Methods

        /**
         * This method returns an indication of whether or not the response
         * has been fully constructed (valid or not).
         *
         * @return
         *     An indication of whether or not the response
         *     has been fully constructed (valid or not) is returned.
         */
        bool IsCompleteOrError() const;

        /**
         * This method generates the data to transmit to the client
         * to return this response to the client.
         *
         * @return
         *     The data to transmit to the client to return
         *     this response to the client is returned.
         */
        std::string Generate() const;
    };

}

#endif /* HTTP_RESPONSE_HPP */
