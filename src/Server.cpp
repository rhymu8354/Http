/**
 * @file Server.cpp
 *
 * This module contains the implementation of the Http::Server class.
 *
 * © 2018 by Richard Walters
 */

#include <condition_variable>
#include <Http/Server.hpp>
#include <inttypes.h>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stddef.h>
#include <stdio.h>
#include <SystemAbstractions/StringExtensions.hpp>
#include <thread>

namespace {

    /**
     * This is the character sequence corresponding to a carriage return (CR)
     * followed by a line feed (LF), which officially delimits each
     * line of an HTTP request.
     */
    const std::string CRLF("\r\n");

    /**
     * This is the maximum allowed request body size.
     */
    constexpr size_t MAX_CONTENT_LENGTH = 10000000;

    /**
     * This is the default maximum length allowed for a request header line.
     */
    constexpr size_t DEFAULT_HEADER_LINE_LIMIT = 1000;

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
        if (request->method.empty()) {
            return false;
        }

        // Parse the target URI.
        const auto targetDelimiter = requestLine.find(' ', methodDelimiter + 1);
        const auto targetLength = targetDelimiter - methodDelimiter - 1;
        if (targetLength == 0) {
            return false;
        }
        if (
            !request->target.ParseFromString(
                requestLine.substr(
                    methodDelimiter + 1,
                    targetLength
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
         * @param[in] server
         *     This is the server that owns this connection.
         *
         * @return
         *     The request parsed from the reassembly buffer is returned.
         *
         * @retval nullptr
         *     This is returned if no request could be parsed from the
         *     reassembly buffer.
         */
        std::shared_ptr< Http::Server::Request > TryRequestAssembly(
            Http::Server* server
        ) {
            size_t messageEnd;
            const auto request = server->ParseRequest(
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
         * This refers back to the server whose private properties
         * are stored here.
         */
        Server* server = nullptr;

        /**
         * This holds all configuration items for the server.
         */
        std::map< std::string, std::string > configuration;

        /**
         * This is the maximum number of characters allowed on any header line
         * of an HTTP request.
         */
        size_t headerLineLimit = DEFAULT_HEADER_LINE_LIMIT;

        /**
         * This is the transport layer currently bound.
         */
        std::shared_ptr< ServerTransport > transport;

        /**
         * These are the currently active client connections.
         */
        std::set< std::shared_ptr< ConnectionState > > activeConnections;

        /**
         * These are the client connections that have broken and will
         * be destroyed by the reaper thread.
         */
        std::set< std::shared_ptr< ConnectionState > > brokenConnections;

        /**
         * This is a helper object used to generate and publish
         * diagnostic messages.
         */
        SystemAbstractions::DiagnosticsSender diagnosticsSender;

        /**
         * This is a worker thread whose sole job is to clear the
         * brokenConnections set.  The reason we need to put broken
         * connections there in the first place is because we can't
         * destroy a connection that is in the process of calling
         * us through one of the delegates we gave it.
         */
        std::thread reaper;

        /**
         * This flag indicates whether or not the reaper thread should stop.
         */
        bool stopReaper = false;

        /**
         * This is used to synchronize access to the server.
         */
        std::mutex mutex;

        /**
         * This is used by the reaper thread to wait on any
         * condition that it should cause it to wake up.
         */
        std::condition_variable reaperWakeCondition;

        // Methods

        /**
         * This is the constructor for the structure.
         */
        Impl()
            : diagnosticsSender("Http::Server")
        {
        }

        /**
         * This method is the body of the reaper thread.
         * Until it's told to stop, it simply clears
         * the brokenConnections set whenever it wakes up.
         */
        void Reaper() {
            std::unique_lock< decltype(mutex) > lock(mutex);
            while (!stopReaper) {
                std::set< std::shared_ptr< ConnectionState > > oldBrokenConnections(std::move(brokenConnections));
                brokenConnections.clear();
                {
                    lock.unlock();
                    oldBrokenConnections.clear();
                    lock.lock();
                }
                reaperWakeCondition.wait(
                    lock,
                    [this]{
                        return (
                            stopReaper
                            || !brokenConnections.empty()
                        );
                    }
                );
            }
        }

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
                const auto request = connectionState->TryRequestAssembly(server);
                if (request == nullptr) {
                    break;
                }
                std::string response;
                unsigned int statusCode;
                std::string reasonPhrase;
                if (request->validity == Request::Validity::Valid) {
                    diagnosticsSender.SendDiagnosticInformationFormatted(
                        1, "Received %s request for '%s' from %s",
                        request->method.c_str(),
                        request->target.GenerateString().c_str(),
                        connectionState->connection->GetPeerId().c_str()
                    );
                    const std::string cannedResponse = (
                        "HTTP/1.1 404 Not Found\r\n"
                        "Content-Length: 13\r\n"
                        "Content-Type: text/plain\r\n"
                        "\r\n"
                        "FeelsBadMan\r\n"
                    );
                    response = cannedResponse;
                    statusCode = 404;
                    reasonPhrase = "Not Found";
                } else {
                    const std::string cannedResponse = (
                        "HTTP/1.1 400 Bad Request\r\n"
                        "Content-Length: 13\r\n"
                        "Content-Type: text/plain\r\n"
                        "\r\n"
                        "FeelsBadMan\r\n"
                    );
                    response = cannedResponse;
                    statusCode = 400;
                    reasonPhrase = "Bad Request";
                }
                connectionState->connection->SendData(
                    std::vector< uint8_t >(
                        response.begin(),
                        response.end()
                    )
                );
                diagnosticsSender.SendDiagnosticInformationFormatted(
                    1, "Sent %u '%s' response back to %s",
                    statusCode,
                    reasonPhrase.c_str(),
                    connectionState->connection->GetPeerId().c_str()
                );
                if (request->validity != Request::Validity::Valid) {
                    if (request->validity == Request::Validity::InvalidUnrecoverable) {
                        connectionState->connection->Break(true);
                    }
                    break;
                }
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
            std::lock_guard< decltype(mutex) > lock(mutex);
            diagnosticsSender.SendDiagnosticInformationFormatted(
                2, "New connection from %s",
                connection->GetPeerId().c_str()
            );
            const auto connectionState = std::make_shared< ConnectionState >();
            connectionState->connection = connection;
            (void)activeConnections.insert(connectionState);
            std::weak_ptr< ConnectionState > connectionStateWeak(connectionState);
            connection->SetDataReceivedDelegate(
                [this, connectionStateWeak](std::vector< uint8_t > data){
                    std::lock_guard< decltype(mutex) > lock(mutex);
                    const auto connectionState = connectionStateWeak.lock();
                    if (connectionState == nullptr) {
                        return;
                    }
                    DataReceived(connectionState, data);
                }
            );
            connection->SetBrokenDelegate(
                [this, connectionStateWeak]{
                    std::lock_guard< decltype(mutex) > lock(mutex);
                    const auto connectionState = connectionStateWeak.lock();
                    if (connectionState == nullptr) {
                        return;
                    }
                    diagnosticsSender.SendDiagnosticInformationFormatted(
                        2, "Connection to %s broken by peer",
                        connectionState->connection->GetPeerId().c_str()
                    );
                    (void)brokenConnections.insert(connectionState);
                    reaperWakeCondition.notify_all();
                    (void)activeConnections.erase(connectionState);
                }
            );
        }
    };

    Server::~Server() {
        Demobilize();
        {
            std::lock_guard< decltype(impl_->mutex) > lock(impl_->mutex);
            impl_->stopReaper = true;
            impl_->reaperWakeCondition.notify_all();
        }
        impl_->reaper.join();
    }

    Server::Server()
        : impl_(new Impl)
    {
        impl_->server = this;
        impl_->configuration["HeaderLineLimit"] = SystemAbstractions::sprintf("%zu", impl_->headerLineLimit);
        impl_->reaper = std::thread(&Impl::Reaper, impl_.get());
    }

    SystemAbstractions::DiagnosticsSender::SubscriptionToken Server::SubscribeToDiagnostics(
        SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate delegate,
        size_t minLevel
    ) {
        return impl_->diagnosticsSender.SubscribeToDiagnostics(delegate, minLevel);
    }

    void Server::UnsubscribeFromDiagnostics(SystemAbstractions::DiagnosticsSender::SubscriptionToken subscriptionToken) {
        impl_->diagnosticsSender.UnsubscribeFromDiagnostics(subscriptionToken);
    }

    std::string Server::GetConfigurationItem(const std::string& key) {
        const auto entry = impl_->configuration.find(key);
        if (entry == impl_->configuration.end()) {
            return "";
        } else {
            return entry->second;
        }
    }

    void Server::SetConfigurationItem(
        const std::string& key,
        const std::string& value
    ) {
        impl_->configuration[key] = value;
        if (key == "HeaderLineLimit") {
            size_t newHeaderLineLimit;
            if (
                sscanf(
                    value.c_str(),
                    "%zu",
                    &newHeaderLineLimit
                ) == 1
            ) {
                impl_->diagnosticsSender.SendDiagnosticInformationFormatted(
                    0,
                    "Header line limit changed from %zu to %zu",
                    impl_->headerLineLimit,
                    newHeaderLineLimit
                );
                impl_->headerLineLimit = newHeaderLineLimit;
            }
        }
    }

    bool Server::Mobilize(
        std::shared_ptr< ServerTransport > transport,
        uint16_t port
    ) {
        impl_->transport = transport;
        if (
            impl_->transport->BindNetwork(
                port,
                [this](std::shared_ptr< Connection > connection){
                    impl_->NewConnection(connection);
                }
            )
        ) {
            impl_->diagnosticsSender.SendDiagnosticInformationFormatted(
                3, "Now listening on port %" PRIu16,
                port
            );
        } else {
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
        messageEnd = 0;

        // First, extract and parse the request line.
        const auto requestLineEnd = rawRequest.find(CRLF);
        if (requestLineEnd == std::string::npos) {
            if (rawRequest.length() > impl_->headerLineLimit) {
                request->validity = Request::Validity::InvalidUnrecoverable;
                return request;
            }
            return nullptr;
        }
        const auto requestLineLength = requestLineEnd;
        if (requestLineLength > impl_->headerLineLimit) {
            request->validity = Request::Validity::InvalidUnrecoverable;
            return request;
        }
        const auto requestLine = rawRequest.substr(0, requestLineLength);
        if (!ParseRequestLine(request, requestLine)) {
            request->validity = Request::Validity::InvalidRecoverable;
        }

        // Second, parse the message headers and identify where the body begins.
        const auto headersOffset = requestLineEnd + CRLF.length();
        size_t bodyOffset;
        request->headers.SetLineLimit(impl_->headerLineLimit);
        switch (
            request->headers.ParseRawMessage(
                rawRequest.substr(headersOffset),
                bodyOffset
            )
        ) {
            case MessageHeaders::MessageHeaders::Validity::ValidComplete: {
            } break;

            case MessageHeaders::MessageHeaders::Validity::ValidIncomplete: {
            } return nullptr;

            case MessageHeaders::MessageHeaders::Validity::InvalidRecoverable: {
                request->validity = Request::Validity::InvalidRecoverable;
            } break;

            case MessageHeaders::MessageHeaders::Validity::InvalidUnrecoverable:
            default: {
                request->validity = Request::Validity::InvalidUnrecoverable;
                return request;
            }
        }

        // Check for "Content-Length" header.  If present, use this to
        // determine how many characters should be in the body.
        bodyOffset += headersOffset;
        const auto bytesAvailableForBody = rawRequest.length() - bodyOffset;

        // Finally, extract the body.  If there is a "Content-Length"
        // header, we carefully carve exactly that number of characters
        // out (and bail if we don't have enough).  Otherwise, we just
        // assume the body extends to the end of the raw message.
        if (request->headers.HasHeader("Content-Length")) {
            size_t contentLength;
            if (!ParseSize(request->headers.GetHeaderValue("Content-Length"), contentLength)) {
                request->validity = Request::Validity::InvalidUnrecoverable;
                return request;
            }
            if (contentLength > MAX_CONTENT_LENGTH) {
                request->validity = Request::Validity::InvalidUnrecoverable;
                return request;
            }
            if (contentLength > bytesAvailableForBody) {
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

    void PrintTo(
        const Server::Request::Validity& validity,
        std::ostream* os
    ) {
        switch (validity) {
            case Server::Request::Validity::Valid: {
                *os << "VALID";
            } break;
            case Server::Request::Validity::InvalidRecoverable: {
                *os << "INVALID (recoverable)";
            } break;
            case Server::Request::Validity::InvalidUnrecoverable: {
                *os << "INVALID (not recoverable)";
            } break;
            default: {
                *os << "???";
            };
        }
    }

}
