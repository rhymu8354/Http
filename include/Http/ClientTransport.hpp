#ifndef HTTP_CLIENT_TRANSPORT_HPP
#define HTTP_CLIENT_TRANSPORT_HPP

/**
 * @file ClientTransport.hpp
 *
 * This module declares the Http::ClientTransport interface.
 *
 * © 2018 by Richard Walters
 */

#include "Connection.hpp"

#include <memory>
#include <stdint.h>
#include <string>

namespace Http {

    /**
     * This represents the transport layer requirements of Http::Client.
     * To integrate Http::Client into a larger program, implement this
     * interface in terms of the actual transport layer.
     */
    class ClientTransport {
    public:
        // Methods

        /**
         * This method establishes a new connection to a server with
         * the given address and port number.
         *
         * @note
         *     The object returned does not do anything for the
         *     SetDataReceivedDelegate or SetBrokenDelegate
         *     methods, since the delegates are specified directly
         *     in this method.
         *
         * @param[in] hostNameOrAddress
         *     This is the host name or IP address of the
         *     server to which to connect.
         *
         * @param[in] port
         *     This is the port number of the server to which to connect.
         *
         * @param[in] dataReceivedDelegate
         *     This is the delegate to call whenever data is recevied
         *     from the remote peer.
         *
         * @param[in] dataReceivedDelegate
         *     This is the delegate to call whenever the connection
         *     has been broken.
         *
         * @return
         *     An object representing the new connection is returned.
         *
         * @retval nullptr
         *     This is returned if a connection could not be established.
         */
        virtual std::shared_ptr< Connection > Connect(
            const std::string& hostNameOrAddress,
            uint16_t port,
            Http::Connection::DataReceivedDelegate dataReceivedDelegate,
            Http::Connection::BrokenDelegate brokenDelegate
        ) = 0;
    };

}

#endif /* HTTP_CLIENT_TRANSPORT_HPP */
