#ifndef HTTP_CLIENT_HPP
#define HTTP_CLIENT_HPP

/**
 * @file Client.hpp
 *
 * This module declares the Http::Client class.
 *
 * © 2018 by Richard Walters
 */

#include <memory>
#include <MessageHeaders/MessageHeaders.hpp>
#include <string>

namespace Http {

    /**
     * This is part of the Http library, which implements
     * [RFC 7230](https://tools.ietf.org/html/rfc7230),
     * "Hypertext Transfer Protocol (HTTP/1.1): Message Syntax and Routing".
     *
     * This class is used to generate HTTP requests (for web servers)
     * and parse HTTP responses received back from web servers.
     */
    class Client {
        // Types
    public:
        /**
         * This represents an overall HTTP response given to a client,
         * decomposed into its various elements.
         */
        struct Response {
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
        };

        // Lifecycle management
    public:
        ~Client();
        Client(const Client&) = delete;
        Client(Client&&) = delete;
        Client& operator=(const Client&) = delete;
        Client& operator=(Client&&) = delete;

        // Public methods
    public:
        /**
         * This is the default constructor.
         */
        Client();

        /**
         * This method parses the given string as a raw HTTP response message.
         * If the string parses correctly, the equivalent Response is returned.
         * Otherwise, nullptr is returned.
         *
         * @param[in] rawResponse
         *     This is the raw HTTP response message as a single string.
         *
         * @return
         *     The Response equivalent to the given raw HTTP response string
         *     is returned.
         *
         * @retval nullptr
         *     This is returned if the given rawResponse did not parse correctly.
         */
        std::shared_ptr< Response > ParseResponse(const std::string& rawResponse);

        /**
         * This method parses the given string as a raw HTTP response message.
         * If the string parses correctly, the equivalent Response is returned.
         * Otherwise, nullptr is returned.
         *
         * @param[in] rawResponse
         *     This is the raw HTTP response message as a single string.
         *
         * @param[out] messageEnd
         *     This is where to store a count of the number of characters
         *     that actually made up the response message.  Presumably,
         *     any characters past this point belong to another message or
         *     are outside the scope of HTTP.
         *
         * @return
         *     The Response equivalent to the given raw HTTP response string
         *     is returned.
         *
         * @retval nullptr
         *     This is returned if the given rawResponse did not parse correctly.
         */
        std::shared_ptr< Response > ParseResponse(
            const std::string& rawResponse,
            size_t& messageEnd
        );

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
        std::unique_ptr< struct Impl > impl_;
    };

}

#endif /* HTTP_CLIENT_HPP */
