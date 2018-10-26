#ifndef HTTP_CLIENT_HPP
#define HTTP_CLIENT_HPP

/**
 * @file Client.hpp
 *
 * This module declares the Http::Client class.
 *
 * © 2018 by Richard Walters
 */

#include "ClientTransport.hpp"
#include "Request.hpp"
#include "Response.hpp"
#include "TimeKeeper.hpp"

#include <chrono>
#include <memory>
#include <MessageHeaders/MessageHeaders.hpp>
#include <ostream>
#include <stddef.h>
#include <string>
#include <SystemAbstractions/DiagnosticsSender.hpp>

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
        // Public Properties

        /**
         * This is the default amount of time that can pass without receiving
         * any data at all from a server, before the client considers the
         * connection to be timed out.
         */
        static constexpr double DEFAULT_REQUEST_TIMEOUT_SECONDS = 10.0;

        /**
         * This is the default amount of time that can pass without a transaction
         * reusing a persistent connection, before the client closes
         * the connection due to inactivity.
         */
        static constexpr double DEFAULT_INACTIVITY_INTERVAL_SECONDS = 60.0;

        // Types
    public:
        /**
         * This structure holds all of the configuration items
         * and dependency objects needed by the client when it's
         * mobilized.
         */
        struct MobilizationDependencies {
            /**
             * This is the transport layer implementation to use.
             */
            std::shared_ptr< ClientTransport > transport;

            /**
             * This is the object used to track time in the client.
             */
            std::shared_ptr< TimeKeeper > timeKeeper;

            /**
             * This is the amount of time after a request is made
             * of a server, before the transaction is considered timed out
             * if no part of a response has been received.
             */
            double requestTimeoutSeconds = DEFAULT_REQUEST_TIMEOUT_SECONDS;

            /**
             * This is the amount of time, after a transaction is completed,
             * that a persistent connection is closed if another transaction
             * does not reuse the connection.
             */
            double inactivityInterval = DEFAULT_INACTIVITY_INTERVAL_SECONDS;
        };

        /**
         * This represents the state of a resource request made through
         * the client.
         */
        struct Transaction {
            // Types

            /**
             * These are the different states that a transaction can be in.
             */
            enum class State {
                /**
                 * The connection to the server is still being established,
                 * or the request is still being sent, or the response
                 * is still being received.
                 */
                InProgress,

                /**
                 * A response has been completely received
                 */
                Completed,

                /**
                 * The connection to the server could not be established.
                 */
                UnableToConnect,

                /**
                 * The server disconnected before a complete response
                 * could be received.
                 */
                Broken,

                /**
                 * The connection timed out waiting for a response
                 * from the server.
                 */
                Timeout,
            };

            // Properties

            /**
             * This indicates how far along the transaction is.
             */
            State state = State::InProgress;

            /**
             * This is either the response obtained from the server,
             * or a substitute made by the client in the case where
             * the transaction could not be completed successfully.
             */
            Http::Response response;

            // Methods

            /**
             * This method can be used to wait for the transaction
             * to complete.
             *
             * @note
             *     This method will return immediately if the state
             *     is not State::InProgress.
             *
             * @param[in] relativeTime
             *     This is the maximum amount of time, in milliseconds,
             *     to wait for the transaction to complete.
             *
             * @return
             *     An indication of whether or not the transaction was
             *     completed in time is returned.
             */
            virtual bool AwaitCompletion(
                const std::chrono::milliseconds& relativeTime
            ) = 0;
        };

        // Lifecycle management
    public:
        ~Client() noexcept;
        Client(const Client&) = delete;
        Client(Client&&) noexcept = delete;
        Client& operator=(const Client&) = delete;
        Client& operator=(Client&&) noexcept = delete;

        // Public methods
    public:
        /**
         * This is the default constructor.
         */
        Client();

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
         *     A function is returned which may be called
         *     to terminate the subscription.
         */
        SystemAbstractions::DiagnosticsSender::UnsubscribeDelegate SubscribeToDiagnostics(
            SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate delegate,
            size_t minLevel = 0
        );

        /**
         * This method will set up the client with its dependencies,
         * preparing it to be able to issue requests to servers.
         *
         * @param[in] deps
         *     These are all of the configuration items and dependency objects
         *     needed by the client when it's mobilized.
         */
        void Mobilize(const MobilizationDependencies& deps);

        /**
         * This method asynchronously posts the given request for a resource
         * on the Internet.
         *
         * @param[in] request
         *     This is the request to make.  The server's address
         *     is obtained from the request's target URI.
         *
         * @param[in] persistConnection
         *     This flag indicates whether or not the connection used to
         *     communicate with the server should be kept open after the
         *     request, possibly to be reused in subsequent requests.
         *
         * @return
         *     An object representing the resource request is returned.
         */
        std::shared_ptr< Transaction > Request(
            Http::Request request,
            bool persistConnection = true
        );

        /**
         * This method stops processing of server connections,
         * and releases the transport layer, returning the client back to the
         * state it was in before Mobilize was called.
         */
        void Demobilize();

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
        std::unique_ptr< Impl > impl_;
    };

    /**
     * This is a support function for Google Test to print out
     * values of the Http::Client::Transaction::State class.
     *
     * @param[in] state
     *     This is the HTTP client transaction state value to print.
     *
     * @param[in] os
     *     This points to the stream to which to print the
     *     HTTP client transaction state value.
     */
    void PrintTo(
        const Http::Client::Transaction::State& state,
        std::ostream* os
    );

}

#endif /* HTTP_CLIENT_HPP */
