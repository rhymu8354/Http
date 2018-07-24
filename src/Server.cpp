/**
 * @file Server.cpp
 *
 * This module contains the implementation of the Http::Server class.
 *
 * © 2018 by Richard Walters
 */

#include <condition_variable>
#include <deque>
#include <Http/Server.hpp>
#include <inttypes.h>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stddef.h>
#include <stdio.h>
#include <string>
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
     * This is used to record what resources are currently supported
     * by the server, and through which handler delegates.
     */
    struct ResourceSpace {
        /**
         * This is the name of the resource space, used as the key
         * to find it amongst all the subspaces of the superspace.
         */
        std::string name;

        /**
         * This is the delegate to call to handle any resource requests
         * within this space.  If nullptr, the space is divided into
         * subspaces.
         */
        Http::Server::ResourceDelegate handler;

        /**
         * If the space is divided into subspaces, these are the
         * subspaces which have currently registered handler delegates.
         */
        std::map< std::string, std::shared_ptr< ResourceSpace > > subspaces;

        /**
         * This points back to the resource superspace containing
         * this subspace.
         */
        std::weak_ptr< ResourceSpace > superspace;
    };

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

        /**
         * This is the state of the next request, while it's still
         * being received and parsed.
         */
        std::shared_ptr< Http::Server::Request > nextRequest = std::make_shared< Http::Server::Request >();
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
         * This represents the entire space of resources under this server.
         */
        std::shared_ptr< ResourceSpace > resources;

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
         * This method is called one or more times to incrementally parse
         * a raw HTTP request message.  For the first call, pass in a newly-
         * constructed request object, and the beginning of the raw
         * HTTP request message.  Contine calling with the same request
         * object and subsequent pieces of the raw HTTP request message,
         * until the request is fully parsed, as indicated by its
         * requestParsingState transitioning to RequestParsingState::Complete,
         * RequestParsingState::InvalidRecoverable, or
         * RequestParsingState::InvalidUnrecoverable.
         *
         * @param[in,out] request
         *     This is the request being parsed.
         *
         * @param[in] nextRawRequestPart
         *     This is the next part of the raw HTTP request message.
         *
         * @return
         *     A count of the number of characters that were taken from
         *     the given input string is returned. Presumably,
         *     any characters past this point belong to another message or
         *     are outside the scope of HTTP.
         */
        size_t ParseRequest(
            std::shared_ptr< Request > request,
            const std::string& nextRawRequestPart
        ) {
            // Count the number of characters incorporated into
            // the request object.
            size_t messageEnd = 0;

            // First, extract and parse the request line.
            if (request->state == Request::State::RequestLine) {
                const auto requestLineEnd = nextRawRequestPart.find(CRLF);
                if (requestLineEnd == std::string::npos) {
                    if (nextRawRequestPart.length() > headerLineLimit) {
                        request->state = Request::State::Error;
                        return messageEnd;
                    }
                    return messageEnd;
                }
                const auto requestLineLength = requestLineEnd;
                if (requestLineLength > headerLineLimit) {
                    request->state = Request::State::Error;
                    return messageEnd;
                }
                const auto requestLine = nextRawRequestPart.substr(0, requestLineLength);
                messageEnd = requestLineEnd + CRLF.length();
                request->state = Request::State::Headers;
                request->valid = ParseRequestLine(request, requestLine);
            }

            // Second, parse the message headers and identify where the body begins.
            if (request->state == Request::State::Headers) {
                request->headers.SetLineLimit(headerLineLimit);
                size_t bodyOffset;
                const auto headersState = request->headers.ParseRawMessage(
                    nextRawRequestPart.substr(messageEnd),
                    bodyOffset
                );
                messageEnd += bodyOffset;
                switch (headersState) {
                    case MessageHeaders::MessageHeaders::State::Complete: {
                        // Done with parsing headers; next will be body.
                        if (!request->headers.IsValid()) {
                            request->valid = false;
                        }
                        request->state = Request::State::Body;

                        // Check for "Host" header.
                        if (request->headers.HasHeader("Host")) {
                            const auto requestHost = request->headers.GetHeaderValue("Host");
                            auto serverHost = configuration["host"];
                            if (serverHost.empty()) {
                                serverHost = requestHost;
                            }
                            auto targetHost = request->target.GetHost();
                            if (targetHost.empty()) {
                                targetHost = serverHost;
                            }
                            if (
                                (requestHost != targetHost)
                                || (requestHost != serverHost)
                            ) {
                                request->valid = false;
                            }
                        } else {
                            request->valid = false;
                        }
                    } break;

                    case MessageHeaders::MessageHeaders::State::Incomplete: {
                    } return messageEnd;

                    case MessageHeaders::MessageHeaders::State::Error:
                    default: {
                        request->state = Request::State::Error;
                        return messageEnd;
                    }
                }
            }

            // Finally, extract the body.
            if (request->state == Request::State::Body) {
                // Check for "Content-Length" header.  If present, use this to
                // determine how many characters should be in the body.
                const auto bytesAvailableForBody = nextRawRequestPart.length() - messageEnd;

                // If there is a "Content-Length"
                // header, we carefully carve exactly that number of characters
                // out (and bail if we don't have enough).  Otherwise, we just
                // assume the body extends to the end of the raw message.
                if (request->headers.HasHeader("Content-Length")) {
                    size_t contentLength;
                    if (!ParseSize(request->headers.GetHeaderValue("Content-Length"), contentLength)) {
                        request->state = Request::State::Error;
                        return messageEnd;
                    }
                    if (contentLength > MAX_CONTENT_LENGTH) {
                        request->state = Request::State::Error;
                        return messageEnd;
                    }
                    if (contentLength > bytesAvailableForBody) {
                        request->state = Request::State::Body;
                        return messageEnd;
                    } else {
                        request->body = nextRawRequestPart.substr(messageEnd, contentLength);
                        messageEnd += contentLength;
                        request->state = Request::State::Complete;
                    }
                } else {
                    request->body.clear();
                    request->state = Request::State::Complete;
                }
            }
            return messageEnd;
        }

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
        std::shared_ptr< Request > TryRequestAssembly(
            std::shared_ptr< ConnectionState > connectionState
        ) {
            const auto charactersAccepted = ParseRequest(
                connectionState->nextRequest,
                connectionState->reassemblyBuffer
            );
            connectionState->reassemblyBuffer.erase(
                connectionState->reassemblyBuffer.begin(),
                connectionState->reassemblyBuffer.begin() + charactersAccepted
            );
            if (!connectionState->nextRequest->IsCompleteOrError()) {
                return nullptr;
            }
            const auto request = connectionState->nextRequest;
            connectionState->nextRequest = std::make_shared< Request >();
            return request;
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
                const auto request = TryRequestAssembly(connectionState);
                if (request == nullptr) {
                    break;
                }
                std::string responseText;
                unsigned int statusCode;
                std::string reasonPhrase;
                if (
                    (request->state == Request::State::Complete)
                    && request->valid
                ) {
                    diagnosticsSender.SendDiagnosticInformationFormatted(
                        1, "Received %s request for '%s' from %s",
                        request->method.c_str(),
                        request->target.GenerateString().c_str(),
                        connectionState->connection->GetPeerId().c_str()
                    );
                    const auto originalResourcePath = request->target.GetPath();
                    std::deque< std::string > resourcePath(
                        originalResourcePath.begin(),
                        originalResourcePath.end()
                    );
                    if (
                        !resourcePath.empty()
                        && (resourcePath.front() == "")
                    ) {
                        (void)resourcePath.pop_front();
                    }
                    std::shared_ptr< ResourceSpace > resource = resources;
                    while (
                        (resource != nullptr)
                        && !resourcePath.empty()
                    ) {
                        const auto subspaceEntry = resource->subspaces.find(resourcePath.front());
                        if (subspaceEntry == resource->subspaces.end()) {
                            break;
                        } else {
                            resource = subspaceEntry->second;
                            resourcePath.pop_front();
                        }
                    }
                    if (
                        (resource != nullptr)
                        && (resource->handler != nullptr)
                    ) {
                        request->target.SetPath({ resourcePath.begin(), resourcePath.end() });
                        const auto response = resource->handler(request);
                        responseText = response->Generate();
                        statusCode = response->statusCode;
                        reasonPhrase = response->reasonPhrase;
                    } else {
                        const std::string cannedResponse = (
                            "HTTP/1.1 404 Not Found\r\n"
                            "Content-Length: 13\r\n"
                            "Content-Type: text/plain\r\n"
                            "\r\n"
                            "FeelsBadMan\r\n"
                        );
                        responseText = cannedResponse;
                        statusCode = 404;
                        reasonPhrase = "Not Found";
                    }
                } else {
                    const std::string cannedResponse = (
                        "HTTP/1.1 400 Bad Request\r\n"
                        "Content-Length: 13\r\n"
                        "Content-Type: text/plain\r\n"
                        "\r\n"
                        "FeelsBadMan\r\n"
                    );
                    responseText = cannedResponse;
                    statusCode = 400;
                    reasonPhrase = "Bad Request";
                }
                connectionState->connection->SendData(
                    std::vector< uint8_t >(
                        responseText.begin(),
                        responseText.end()
                    )
                );
                diagnosticsSender.SendDiagnosticInformationFormatted(
                    1, "Sent %u '%s' response back to %s",
                    statusCode,
                    reasonPhrase.c_str(),
                    connectionState->connection->GetPeerId().c_str()
                );
                if (request->state == Request::State::Complete) {
                    const auto connectionTokens = request->headers.GetHeaderMultiValue("Connection");
                    bool closeRequested = false;
                    for (const auto& connectionToken: connectionTokens) {
                        if (connectionToken == "close") {
                            closeRequested = true;
                            break;
                        }
                    }
                    if (closeRequested) {
                        connectionState->connection->Break(true);
                    }
                } else {
                    if (request->state == Request::State::Error) {
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

    bool Server::Request::IsCompleteOrError() const {
        return (
            (state == State::Complete)
            || (state == State::Error)
        );
    }

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

    auto Server::RegisterResource(
        const std::vector< std::string >& resourceSubspacePath,
        ResourceDelegate resourceDelegate
    ) -> UnregistrationDelegate {
        std::shared_ptr< ResourceSpace > space = impl_->resources;
        if (space == nullptr) {
            space = impl_->resources = std::make_shared< ResourceSpace >();
        }
        for (const auto& pathSegment: resourceSubspacePath) {
            if (space->handler != nullptr) {
                return nullptr;
            }
            std::shared_ptr< ResourceSpace > subspace;
            auto subspacesEntry = space->subspaces.find(pathSegment);
            if (subspacesEntry == space->subspaces.end()) {
                subspace = space->subspaces[pathSegment] = std::make_shared< ResourceSpace >();
                subspace->name = pathSegment;
                subspace->superspace = space;
            } else {
                subspace = subspacesEntry->second;
            }
            space = subspace;
        }
        if (
            (space->handler == nullptr)
            && space->subspaces.empty()
        ) {
            space->handler = resourceDelegate;
            return [this, space]{
                auto currentSpace = space;
                for (;;) {
                    auto superspace = currentSpace->superspace.lock();
                    if (superspace == nullptr) {
                        impl_->resources = nullptr;
                        break;
                    } else {
                        (void)superspace->subspaces.erase(currentSpace->name);
                    }
                    if (superspace->subspaces.empty()) {
                        currentSpace = superspace;
                    } else {
                        break;
                    }
                }
            };
        } else {
            return nullptr;
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
        auto request = std::make_shared< Request >();
        messageEnd = impl_->ParseRequest(request, rawRequest);
        if (request->IsCompleteOrError()) {
            return request;
        } else {
            return nullptr;
        }
    }

    void PrintTo(
        const Server::Request::State& state,
        std::ostream* os
    ) {
        switch (state) {
            case Server::Request::State::RequestLine: {
                *os << "Constructing Request line";
            } break;
            case Server::Request::State::Headers: {
                *os << "Constructing Headers";
            } break;
            case Server::Request::State::Body: {
                *os << "Constructing Body";
            } break;
            case Server::Request::State::Complete: {
                *os << "COMPLETE";
            } break;
            case Server::Request::State::Error: {
                *os << "ERROR";
            } break;
            default: {
                *os << "???";
            };
        }
    }

}
