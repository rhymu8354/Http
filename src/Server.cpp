/**
 * @file Server.cpp
 *
 * This module contains the implementation of the Http::Server class.
 *
 * © 2018 by Richard Walters
 */

#include <Http/Server.hpp>
#include <memory>
#include <set>
#include <stddef.h>

namespace {

    /**
     * This is the character sequence corresponding to a carriage return (CR)
     * followed by a line feed (LF), which officially delimits each
     * line of an HTTP request.
     */
    const std::string CRLF("\r\n");

    /**
     * This function parses the given string as a size
     * integer, detecting invalid characters, overflow, etc.
     *
     * @param[in] numberString
     *     This is the string containing the number to parse.
     *
     * @param[out] number
     *     This is where to store the number parsed.
     *
     * @return
     *     An indication of whether or not the number was parsed
     *     successfully is returned.
     */
    bool ParseSize(
        const std::string& numberString,
        size_t& number
    ) {
        number = 0;
        for (auto c: numberString) {
            if (
                (c < '0')
                || (c > '9')
            ) {
                return false;
            }
            auto previousNumber = number;
            number *= 10;
            number += (uint16_t)(c - '0');
            if (
                (number / 10) != previousNumber
            ) {
                return false;
            }
        }
        return true;
    }

    /**
     * This method parses the method, target URI, and protocol identifier
     * from the given request line.
     *
     * @param[in] request
     *     This is the request in which to store the parsed method and
     *     target URI.
     *
     * @param[in] requestLine
     *     This is the raw request line string to parse.
     *
     * @return
     *     An indication of whether or not the request line
     *     was successfully parsed is returned.
     */
    bool ParseRequestLine(
        std::shared_ptr< Http::Server::Request > request,
        const std::string& requestLine
    ) {
        // Parse the method.
        const auto methodDelimiter = requestLine.find(' ');
        if (methodDelimiter == std::string::npos) {
            return false;
        }
        request->method = requestLine.substr(0, methodDelimiter);

        // Parse the target URI.
        const auto targetDelimiter = requestLine.find(' ', methodDelimiter + 1);
        if (
            !request->target.ParseFromString(
                requestLine.substr(
                    methodDelimiter + 1,
                    targetDelimiter - methodDelimiter - 1
                )
            )
        ) {
            return false;
        }

        // Parse the protocol.
        const auto protocol = requestLine.substr(targetDelimiter + 1);
        return (protocol == "HTTP/1.1");
    }

    /**
     * This structure holds onto all state information the server has
     * about a single connection from a client.
     */
    struct ConnectionState {
        // Properties

        /**
         * This is the transport interface of the connection.
         */
        std::shared_ptr< Http::Connection > connection;

        /**
         * This buffer is used to reassemble fragmented HTTP requests
         * received from the client.
         */
        std::string reassemblyBuffer;

        // Methods

        /**
         * This method appends the given data to the end of the reassembly
         * buffer, and then attempts to parse a request out of it.
         *
         * @return
         *     The request parsed from the reassembly buffer is returned.
         *
         * @retval nullptr
         *     This is returned if no request could be parsed from the
         *     reassembly buffer.
         */
        std::shared_ptr< Http::Server::Request > TryRequestAssembly() {
            size_t messageEnd;
            const auto request = Http::Server::ParseRequest(
                reassemblyBuffer,
                messageEnd
            );
            if (request == nullptr) {
                return nullptr;
            }
            reassemblyBuffer.erase(
                reassemblyBuffer.begin(),
                reassemblyBuffer.begin() + messageEnd
            );
            return request;
        }

    };

}

namespace Http {

    /**
     * This contains the private properties of a Server instance.
     */
    struct Server::Impl {
        // Properties

        /**
         * This is the transport layer currently bound.
         */
        std::shared_ptr< ServerTransport > transport;

        /**
         * These are the currently active client connections.
         */
        std::set< std::shared_ptr< ConnectionState > > activeConnections;

        // Methods

        /**
         * This method is called when new data is received from a connection.
         *
         * @param[in] connectionState
         *     This is the state of the connection from which data was received.
         *
         * @param[in] data
         *     This is a copy of the data that was received from the connection.
         */
        void DataReceived(
            std::shared_ptr< ConnectionState > connectionState,
            std::vector< uint8_t > data
        ) {
            connectionState->reassemblyBuffer += std::string(data.begin(), data.end());
            for (;;) {
                const auto request = connectionState->TryRequestAssembly();
                if (request == nullptr) {
                    break;
                }
                const std::string cannedResponse = (
                    "HTTP/1.1 404 Not Found\r\n"
                    "Content-Length: 13\r\n"
                    "Content-Type: text/plain\r\n"
                    "\r\n"
                    "FeelsBadMan\r\n"
                );
                connectionState->connection->SendData(
                    std::vector< uint8_t >(
                        cannedResponse.begin(),
                        cannedResponse.end()
                    )
                );
            }
        }

        /**
         * This method is called when a new connection has been
         * established for the server.
         *
         * @param[in] connection
         *     This is the new connection has been established for the server.
         */
        void NewConnection(std::shared_ptr< Connection > connection) {
            const auto connectionState = std::make_shared< ConnectionState >();
            connectionState->connection = connection;
            (void)activeConnections.insert(connectionState);
            std::weak_ptr< ConnectionState > connectionStateWeak(connectionState);
            connection->SetDataReceivedDelegate(
                [this, connectionStateWeak](std::vector< uint8_t > data){
                    const auto connectionState = connectionStateWeak.lock();
                    if (connectionState == nullptr) {
                        return;
                    }
                    DataReceived(connectionState, data);
                }
            );
        }
    };

    Server::~Server() {
        Demobilize();
    }

    Server::Server()
        : impl_(new Impl)
    {
    }

    bool Server::Mobilize(
        std::shared_ptr< ServerTransport > transport,
        uint16_t port
    ) {
        impl_->transport = transport;
        if (
            !impl_->transport->BindNetwork(
                port,
                [this](std::shared_ptr< Connection > connection){
                    impl_->NewConnection(connection);
                }
            )
        ) {
            impl_->transport = nullptr;
            return false;
        }
        return true;
    }

    void Server::Demobilize() {
        if (impl_->transport != nullptr) {
            impl_->transport->ReleaseNetwork();
            impl_->transport = nullptr;
        }
    }

    auto Server::ParseRequest(const std::string& rawRequest) -> std::shared_ptr< Request > {
        size_t messageEnd;
        return ParseRequest(rawRequest, messageEnd);
    }

    auto Server::ParseRequest(
        const std::string& rawRequest,
        size_t& messageEnd
    ) -> std::shared_ptr< Request > {
        const auto request = std::make_shared< Request >();

        // First, extract and parse the request line.
        const auto requestLineEnd = rawRequest.find(CRLF);
        if (requestLineEnd == std::string::npos) {
            return nullptr;
        }
        const auto requestLine = rawRequest.substr(0, requestLineEnd);
        if (!ParseRequestLine(request, requestLine)) {
            return nullptr;
        }

        // Second, parse the message headers and identify where the body begins.
        const auto headersOffset = requestLineEnd + CRLF.length();
        size_t bodyOffset;
        if (
            !request->headers.ParseRawMessage(
                rawRequest.substr(headersOffset),
                bodyOffset
            )
        ) {
            return nullptr;
        }

        // Check for "Content-Length" header.  If present, use this to
        // determine how many characters should be in the body.
        bodyOffset += headersOffset;
        const auto maxContentLength = rawRequest.length() - bodyOffset;

        // Finally, extract the body.  If there is a "Content-Length"
        // header, we carefully carve exactly that number of characters
        // out (and bail if we don't have enough).  Otherwise, we just
        // assume the body extends to the end of the raw message.
        if (request->headers.HasHeader("Content-Length")) {
            size_t contentLength;
            if (!ParseSize(request->headers.GetHeaderValue("Content-Length"), contentLength)) {
                return nullptr;
            }
            if (contentLength > maxContentLength) {
                return nullptr;
            } else {
                request->body = rawRequest.substr(bodyOffset, contentLength);
                messageEnd = bodyOffset + contentLength;
            }
        } else {
            request->body.clear();
            messageEnd = bodyOffset;
        }
        return request;
    }

}
