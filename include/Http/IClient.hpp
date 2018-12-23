#ifndef HTTP_I_CLIENT_HPP
#define HTTP_I_CLIENT_HPP

/**
 * @file IClient.hpp
 *
 * This module declares the Http::IClient interface.
 *
 * © 2018 by Richard Walters
 */

#include "Connection.hpp"
#include "Request.hpp"
#include "Response.hpp"

#include <chrono>
#include <functional>
#include <SystemAbstractions/DiagnosticsSender.hpp>

namespace Http {

    /**
     * This is part of the Http library, which implements
     * [RFC 7230](https://tools.ietf.org/html/rfc7230),
     * "Hypertext Transfer Protocol (HTTP/1.1): Message Syntax and Routing".
     *
     * This is the public interface to a class used to generate HTTP requests
     * (for web servers) and parse HTTP responses received back from web
     * servers.
     */
    class IClient {
    public:
        // Types

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

            /**
             * This method can be used to wait for the transaction
             * to complete.
             *
             * @note
             *     This method will return immediately if the state
             *     is not State::InProgress.
             */
            virtual void AwaitCompletion() = 0;

            /**
             * Set a delegate to be called once the transaction is completed.
             *
             * @param[in] completionDelegate
             *     This is the delegate to call once the transaction is
             *     completed.
             */
            virtual void SetCompletionDelegate(
                std::function< void() > completionDelegate
            ) = 0;
        };

        /**
         * This is the type of function which the user can provide for the
         * client to call if the server upgrades the connection in response
         * to a request.
         *
         * @param[in] response
         *     This is the response received back from the server which
         *     indicates that the connection is upgraded.
         *
         * @param[in] connection
         *     This is the connection upgraded by the server.  The user should
         *     retain a reference to this conneciton and call
         *     SetDataReceivedDelegate and SetBrokenDelegate on this
         *     connection before returning, in order to receive subsequent
         *     callbacks when more data is received or the connection is
         *     broken after the connection is upgraded.
         *
         * @param[in] trailer
         *     This holds any data that has already been received by the client
         *     from the connection, but that came after the upgrade response.
         *     This should be treated according to the upgraded protocol.
         */
        typedef std::function<
            void(
                const Http::Response& response,
                std::shared_ptr< Http::Connection > connection,
                const std::string& trailer
            )
        > UpgradeDelegate;

        // Methods

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
        virtual SystemAbstractions::DiagnosticsSender::UnsubscribeDelegate SubscribeToDiagnostics(
            SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate delegate,
            size_t minLevel = 0
        ) = 0;

        /**
         * This method asynchronously posts the given request for a resource
         * on the Internet.
         *
         * @note
         *     If the server response indicates that the connection protocol
         *     is being upgraded, and an upgrade delegate is given, the
         *     connection is handed to the upgrade delegate, and the
         *     persistConnection flag has no effect; the connection is released
         *     by the client and neither closed nor reused in subsequent
         *     requests.  The user may either await the completion of the
         *     transaction to obtain the response, obtain the response in
         *     the upgrade delegate (where it's passed as a parameter),
         *     or both.
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
         * @param[in] upgradeDelegate
         *     This is an optional function to call if the connection protocol
         *     is upgraded by the server.
         *
         * @return
         *     An object representing the resource request is returned.
         */
        virtual std::shared_ptr< Transaction > Request(
            Http::Request request,
            bool persistConnection = true,
            UpgradeDelegate upgradeDelegate = nullptr
        ) = 0;
    };

}

#endif /* HTTP_I_CLIENT_HPP */
