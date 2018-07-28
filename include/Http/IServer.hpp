#ifndef HTTP_I_SERVER_HPP
#define HTTP_I_SERVER_HPP

/**
 * @file IServer.hpp
 *
 * This module declares the Http::IServer interface.
 *
 * © 2018 by Richard Walters
 */

#include "ServerTransport.hpp"

#include <functional>
#include <Http/Client.hpp>
#include <ostream>
#include <memory>
#include <MessageHeaders/MessageHeaders.hpp>
#include <stdint.h>
#include <string>
#include <SystemAbstractions/DiagnosticsSender.hpp>
#include <Uri/Uri.hpp>

namespace Http {

    /**
     * This is public interface to the HTTP server from plug-ins
     * and other modules that are outside of the HTTP server.
     */
    class IServer {
    public:
        // Types

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
         * This is type of function which can be registered to handle
         * HTTP requests.
         *
         * @param[in] request
         *     This is the request to apply to the resource.
         *
         * @return
         *     The response to be returned to the client is returned.
         */
        typedef std::function<
            std::shared_ptr< Client::Response >(
                std::shared_ptr< Request > request
            )
        > ResourceDelegate;

        /**
         * This is the type of function returned by RegisterResource,
         * to be called when the resource should be unregistered
         * from the server.
         */
        typedef std::function< void() > UnregistrationDelegate;

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
        virtual SystemAbstractions::DiagnosticsSender::SubscriptionToken SubscribeToDiagnostics(
            SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate delegate,
            size_t minLevel = 0
        ) = 0;

        /**
         * This method terminates a subscription previously formed
         * by calling the SubscribeToDiagnostics method.
         *
         * @param[in] subscriptionToken
         *     This is the token returned from SubscribeToDiagnostics
         *     when the subscription was formed.
         */
        virtual void UnsubscribeFromDiagnostics(SystemAbstractions::DiagnosticsSender::SubscriptionToken subscriptionToken) = 0;

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
        virtual std::string GetConfigurationItem(const std::string& key) = 0;

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
        virtual void SetConfigurationItem(
            const std::string& key,
            const std::string& value
        ) = 0;

        /**
         * This method registers the given delegate to be called in order
         * to generate a response for any request that comes in to the server
         * with a target URI which identifies a resource within the given
         * resource subspace of the server.
         *
         * @param[in] resourceSubspacePath
         *     This identifies the subspace of resources that we want
         *     the given delegate to be responsible for handling.
         *
         * @param[in] resourceDelegate
         *     This is the function to call in order to apply the given
         *     request and come up with a response when the request
         *     identifies a resource within the given resource subspace
         *     of the server.
         *
         * @return
         *     A function is returned which, if called, revokes
         *     the registration of the resource delegate, so that subsequent
         *     requests to any resource within the registered resource
         *     substate are no longer handled by the
         *     formerly-registered delegate.
         */
        virtual UnregistrationDelegate RegisterResource(
            const std::vector< std::string >& resourceSubspacePath,
            ResourceDelegate resourceDelegate
        ) = 0;
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
        const IServer::Request::State& state,
        std::ostream* os
    );

}

#endif /* HTTP_I_SERVER_HPP */
