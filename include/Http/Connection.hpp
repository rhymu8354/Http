#ifndef HTTP_CONNECTION_HPP
#define HTTP_CONNECTION_HPP

/**
 * @file Connection.hpp
 *
 * This module declares the Http::Connection interface.
 *
 * © 2018 by Richard Walters
 */

#include <functional>
#include <memory>
#include <stdint.h>
#include <string>
#include <vector>

namespace Http {

    /**
     * This represents a single connection between an HTTP server and
     * an HTTP client on a transport layer.
     */
    class Connection {
    public:
        // Types

        /**
         * This is the type of delegate used to deliver received data
         * to the user of this interface.
         *
         * @param[in] data
         *     This is the data that was received from the remote peer.
         */
        typedef std::function< void(const std::vector< uint8_t >& data) > DataReceivedDelegate;

        /**
         * This is the type of delegate used to notify the user that
         * the connection has been broken.
         *
         * @param[in] graceful
         *     This indicates whether or not the peer of connection
         *     has closed the connection gracefully (meaning we can
         *     continue to send our data back to the peer).
         */
        typedef std::function< void(bool graceful) > BrokenDelegate;

        // Methods

        /**
         * This method returns a string that uniquely identifies
         * the peer of this connection in the context of the transport.
         *
         * @return
         *     A string that uniquely identifies the peer of this connection
         *     in the context of the transport is returned.
         */
        virtual std::string GetPeerId() = 0;

        /**
         * This method sets the delegate to call whenever data is recevied
         * from the remote peer.
         *
         * @param[in] dataReceivedDelegate
         *     This is the delegate to call whenever data is recevied
         *     from the remote peer.
         */
        virtual void SetDataReceivedDelegate(DataReceivedDelegate dataReceivedDelegate) = 0;

        /**
         * This method sets the delegate to call whenever the connection
         * has been broken.
         *
         * @param[in] dataReceivedDelegate
         *     This is the delegate to call whenever the connection
         *     has been broken.
         */
        virtual void SetBrokenDelegate(BrokenDelegate brokenDelegate) = 0;

        /**
         * This method sends the given data to the remote peer.
         *
         * @param[in] data
         *     This is the data to send to the remote peer.
         */
        virtual void SendData(const std::vector< uint8_t >& data) = 0;

        /**
         * This method breaks the connection to the remote peer.
         *
         * @param[in] clean
         *     This flag indicates whether or not to attempt to complete
         *     any data transmission still in progress, before breaking
         *     the connection.
         */
        virtual void Break(bool clean) = 0;
    };

}

#endif /* HTTP_CONNECTION_HPP */
