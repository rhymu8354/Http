#ifndef HTTP_SERVER_TRANSPORT_HPP
#define HTTP_SERVER_TRANSPORT_HPP

/**
 * @file ServerTransport.hpp
 *
 * This module declares the Http::ServerTransport interface.
 *
 * © 2018 by Richard Walters
 */

#include "Connection.hpp"

#include <functional>
#include <memory>
#include <stdint.h>

namespace Http {

    /**
     * This represents the transport layer requirements of Http::Server.
     * To integrate Http::Server into a larger program, implement this
     * interface in terms of the actual transport layer.
     */
    class ServerTransport {
    public:
        // Types

        /**
         * This is the type of delegate used to notify the user that
         * a new connection has been established for the server.
         *
         * @param[in] connection
         *     This is the new connection has been established for the server.
         */
        typedef std::function< void(std::shared_ptr< Connection > connection) > NewConnectionDelegate;

        // Methods

        /**
         * This method acquires exclusive access to the given port on
         * all network interfaces, and begins the process of listening for
         * and accepting incoming connections from clients.
         *
         * @param[in] port
         *     This is the public port number to which clients may connect
         *     to establish connections with this server.
         *
         * @param[in] newConnectionDelegate
         *     This is the delegate to call whenever a new connection
         *     has been established for the server.
         *
         * @return
         *     An indication of whether or not the method was successful
         *     is returned.
         */
        virtual bool BindNetwork(
            uint16_t port,
            NewConnectionDelegate newConnectionDelegate
        ) = 0;

        /**
         * This method releases all resources and access that were acquired
         * and held as a result of calling the BindNetwork method.
         */
        virtual void ReleaseNetwork() = 0;
    };

}

#endif /* HTTP_SERVER_TRANSPORT_HPP */
