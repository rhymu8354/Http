#ifndef HTTP_SERVER_HPP
#define HTTP_SERVER_HPP

/**
 * @file Server.hpp
 *
 * This module declares the Http::Server class.
 *
 * © 2018 by Richard Walters
 */

#include "IServer.hpp"
#include "ServerTransport.hpp"
#include "TimeKeeper.hpp"

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
     * This is part of the Http library, which implements
     * [RFC 7230](https://tools.ietf.org/html/rfc7230),
     * "Hypertext Transfer Protocol (HTTP/1.1): Message Syntax and Routing".
     *
     * This class is used to parse incoming HTTP requests (from web
     * browsersor automated programs), route them to handlers that
     * can satisfy the requests, and then generate appropriate HTTP
     * responses to return back to the original HTTP request senders.
     */
    class Server
        : public IServer
    {
        // Types
    public:
        /**
         * This structure holds all of the configuration items
         * and dependency objects needed by the server when it's
         * mobilized.
         */
        struct MobilizationDependencies {
            /**
             * This is the transport layer implementation to use.
             */
            std::shared_ptr< ServerTransport > transport;

            /**
             * This is the object used to track time in the server.
             */
            std::shared_ptr< TimeKeeper > timeKeeper;
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
         * This method will cause the server to bind to the given transport
         * layer and start accepting and processing connections from clients.
         *
         * @param[in] deps
         *     These are all of the configuration items and dependency objects
         *     needed by the server when it's mobilized.
         *
         * @param[in] port
         *     This is the public port number to which clients may connect
         *     to establish connections with this server.
         *
         * @return
         *     An indication of whether or not the method was successful
         *     is returned.
         */
        bool Mobilize(const MobilizationDependencies& deps);

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

        // IServer
    public:
        virtual SystemAbstractions::DiagnosticsSender::UnsubscribeDelegate SubscribeToDiagnostics(
            SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate delegate,
            size_t minLevel = 0
        ) override;
        virtual std::string GetConfigurationItem(const std::string& key) override;
        virtual void SetConfigurationItem(
            const std::string& key,
            const std::string& value
        ) override;
        virtual UnregistrationDelegate RegisterResource(
            const std::vector< std::string >& resourceSubspacePath,
            ResourceDelegate resourceDelegate
        ) override;

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
