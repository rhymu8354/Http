#ifndef HTTP_SERVER_HPP
#define HTTP_SERVER_HPP

/**
 * @file Server.hpp
 *
 * This module declares the Http::Server class.
 *
 * © 2018 by Richard Walters
 */

#include <memory>
#include <MessageHeaders/MessageHeaders.hpp>
#include <string>
#include <Uri/Uri.hpp>

namespace Http {

    /**
     * This is part of the Http library, which implements
     * [RFC 7230](https://tools.ietf.org/html/rfc7230),
     * "Hypertext Transfer Protocol (HTTP/1.1): Message Syntax and Routing".
     *
     * This class is used to parse incoming HTTP requests (from web
     * browsersor automated programs), route them to handlers that
     * can satisfy the requests, and then generate appropriate HTTP
     * responses to return back to the original HTTP request senders.
     */
    class Server {
        // Types
    public:
        /**
         * This represents an overall HTTP server request, decomposed
         * into its various elements.
         */
        struct Request {
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
        };

        // Lifecycle management
    public:
        ~Server();
        Server(const Server&) = delete;
        Server(Server&&) = delete;
        Server& operator=(const Server&) = delete;
        Server& operator=(Server&&) = delete;

        // Public methods
    public:
        /**
         * This is the default constructor.
         */
        Server();

        /**
         * This method parses the given string as a raw HTTP request message.
         * If the string parses correctly, the equivalent Request is returned.
         * Otherwise, nullptr is returned.
         *
         * @param[in] rawRequest
         *     This is the raw HTTP request message as a single string.
         *
         * @return
         *     The Request equivalent to the given raw HTTP request string
         *     is returned.
         *
         * @retval nullptr
         *     This is returned if the given rawRequest did not parse correctly.
         */
        static std::shared_ptr< Request > ParseRequest(const std::string& rawRequest);

        /**
         * This method parses the given string as a raw HTTP request message.
         * If the string parses correctly, the equivalent Request is returned.
         * Otherwise, nullptr is returned.
         *
         * @param[in] rawRequest
         *     This is the raw HTTP request message as a single string.
         *
         * @param[out] messageEnd
         *     This is where to store a count of the number of characters
         *     that actually made up the request message.  Presumably,
         *     any characters past this point belong to another message or
         *     are outside the scope of HTTP.
         *
         * @return
         *     The Request equivalent to the given raw HTTP request string
         *     is returned.
         *
         * @retval nullptr
         *     This is returned if the given rawRequest did not parse correctly.
         */
        static std::shared_ptr< Request > ParseRequest(
            const std::string& rawRequest,
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

#endif /* HTTP_SERVER_HPP */
