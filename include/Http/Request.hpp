#ifndef HTTP_REQUEST_HPP
#define HTTP_REQUEST_HPP

/**
 * @file Request.hpp
 *
 * This module declares the Http::Request structure.
 *
 * © 2018 by Richard Walters
 */

#include <MessageHeaders/MessageHeaders.hpp>
#include <ostream>
#include <string>
#include <Uri/Uri.hpp>

namespace Http {

    /**
     * This represents an overall HTTP server request, decomposed
     * into its various elements.
     */
    struct Request {
        // Types

        /**
         * This type is used to track how much of the request
         * has been constructed so far.
         */
        enum class State {
            /**
             * In this state, we're still waiting to construct
             * the full request line.
             */
            RequestLine,

            /**
             * In this state, we've constructed the request
             * line, and possibly some header lines, but haven't yet
             * constructed all of the header lines.
             */
            Headers,

            /**
             * In this state, we've constructed the request
             * line and headers, and possibly some of the body, but
             * haven't yet constructed all of the body.
             */
            Body,

            /**
             * In this state, the request either fully constructed
             * or is invalid, but the connection from which the request
             * was constructed can remain open to accept another request.
             */
            Complete,

            /**
             * In this state, the connection from which the request
             * was constructed should be closed, either for security
             * reasons, or because it would be impossible
             * or unlikely to receive a valid request after
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
         * This indicates the request method to be performed on the
         * target resource.
         */
        std::string method;

        /**
         * This identifies the target resource upon which to apply
         * the request.
         */
        Uri::Uri target;

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
         * This indicates whether or not the request
         * passed all validity checks when it was parsed,
         * and if not, whether or not the connection can
         * still be used.
         */
        State state = State::RequestLine;

        // Methods

        /**
         * This method returns an indication of whether or not the request
         * has been fully constructed (valid or not).
         *
         * @return
         *     An indication of whether or not the request
         *     has been fully constructed (valid or not) is returned.
         */
        bool IsCompleteOrError() const;
    };

    /**
     * This is a support function for Google Test to print out
     * values of the Request::State class.
     *
     * @param[in] state
     *     This is the server request state value to print.
     *
     * @param[in] os
     *     This points to the stream to which to print the
     *     server request state value.
     */
    void PrintTo(
        const Request::State& state,
        std::ostream* os
    );

}

#endif /* HTTP_REQUEST_HPP */
