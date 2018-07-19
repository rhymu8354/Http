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
         * This method returns the value of the given server
         * configuration item.
         *
         * @param[in] key
         *     This is the key identifying the configuration item
         *     whose value should be returned.
         *
         * @return
         *     The value of the configuration item is returned.
         */
        std::string GetConfigurationItem(const std::string& key);

        /**
         * This method sets the value of the given server configuration item.
         *
         * @param[in] key
         *     This is the key identifying the configuration item
         *     whose value should be set.
         *
         * @param[in] value
         *     This is the value to set for the configuration item.
         */
        void SetConfigurationItem(
            const std::string& key,
            const std::string& value
        );

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
        std::shared_ptr< Request > ParseRequest(const std::string& rawRequest);

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
        std::shared_ptr< Request > ParseRequest(
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
     * values of the Server::Request::State class.
     *
     * @param[in] state
     *     This is the server request state value to print.
     *
     * @param[in] os
     *     This points to the stream to which to print the
     *     server request state value.
     */
    void PrintTo(
        const Server::Request::State& state,
        std::ostream* os
    );

}

#endif /* HTTP_SERVER_HPP */
