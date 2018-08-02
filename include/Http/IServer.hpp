#ifndef HTTP_I_SERVER_HPP
#define HTTP_I_SERVER_HPP

/**
 * @file IServer.hpp
 *
 * This module declares the Http::IServer interface.
 *
 * © 2018 by Richard Walters
 */

#include "Request.hpp"
#include "Response.hpp"

#include <functional>
#include <Http/Client.hpp>
#include <Http/Connection.hpp>
#include <memory>
#include <MessageHeaders/MessageHeaders.hpp>
#include <stddef.h>
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
         * This is type of function which can be registered to handle
         * HTTP requests.
         *
         * @param[in] request
         *     This is the request to apply to the resource.
         *
         * @param[in] connection
         *     This is the connection on which the request was made.
         *
         * @param[in] trailer
         *     This holds any characters that have already been received
         *     by the server but come after the end of the current
         *     request.  A handler that upgrades the connection might want
         *     to interpret these characters within the context of the
         *     upgraded connection.
         *
         * @return
         *     The response to be returned to the client is returned.
         */
        typedef std::function<
            std::shared_ptr< Response >(
                std::shared_ptr< Request > request,
                std::shared_ptr< Connection > connection,
                const std::string& trailer
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

}

#endif /* HTTP_I_SERVER_HPP */
