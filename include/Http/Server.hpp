#ifndef HTTP_SERVER_HPP
#define HTTP_SERVER_HPP

/**
 * @file Server.hpp
 *
 * This module declares the Http::Server class.
 *
 * © 2018 by Richard Walters
 */

#include "ServerTransport.hpp"

#include <ostream>
#include <memory>
#include <MessageHeaders/MessageHeaders.hpp>
#include <stdint.h>
#include <string>
#include <SystemAbstractions/DiagnosticsSender.hpp>
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
            // Types

            /**
             * These are the different validity states
             * that a request can have.
             */
            enum class Validity {
                /**
                 * good request
                 */
                Valid,

                /**
                 * bad request, but server can keep connection
                 */
                InvalidRecoverable,

                /**
                 * bad request, and server should close connection
                 */
                InvalidUnrecoverable,
            };

            // Properties

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
            Validity validity = Validity::Valid;
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
         * This method forms a new subscription to diagnostic
         * messages published by the sender.
         *
         * @param[in] delegate
         *     This is the function to call to deliver messages
         *     to this subscriber.
         *
         * @param[in] minLevel
         *     This is the minimum level of message that this subscriber
         *     desires to receive.
         *
         * @return
         *     A token representing the subscription is returned.
         *     This may be passed to UnsubscribeFromDiagnostics
         *     in order to terminate the subscription.
         */
        SystemAbstractions::DiagnosticsSender::SubscriptionToken SubscribeToDiagnostics(
            SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate delegate,
            size_t minLevel = 0
        );

        /**
         * This method terminates a subscription previously formed
         * by calling the SubscribeToDiagnostics method.
         *
         * @param[in] subscriptionToken
         *     This is the token returned from SubscribeToDiagnostics
         *     when the subscription was formed.
         */
        void UnsubscribeFromDiagnostics(SystemAbstractions::DiagnosticsSender::SubscriptionToken subscriptionToken);

        /**
         * This method will cause the server to bind to the given transport
         * layer and start accepting and processing connections from clients.
         *
         * @param[in] transport
         *     This is the transport layer implementation to use.
         *
         * @param[in] port
         *     This is the public port number to which clients may connect
         *     to establish connections with this server.
         *
         * @return
         *     An indication of whether or not the method was successful
         *     is returned.
         */
        bool Mobilize(
            std::shared_ptr< ServerTransport > transport,
            uint16_t port
        );

        /**
         * This method stops any accepting or processing of client connections,
         * and releases the transport layer, returning the server back to the
         * state it was in before Mobilize was called.
         */
        void Demobilize();

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
         *     This is returned if the given rawRequest is incomplete.
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

    /**
     * This is a support function for Google Test to print out
     * values of the Server::Request::Validity class.
     *
     * @param[in] validity
     *     This is the validity value to print.
     *
     * @param[in] os
     *     This points to the stream to which to print the validity value.
     */
    void PrintTo(
        const Server::Request::Validity& validity,
        std::ostream* os
    );

}

#endif /* HTTP_SERVER_HPP */
