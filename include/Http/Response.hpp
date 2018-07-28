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
        // Properties

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

        // Methods

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
