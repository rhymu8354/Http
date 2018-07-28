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
     * This is the default public port number to which clients may connect
     * to establish connections with this server.
     */
    constexpr uint16_t DEFAULT_PORT_NUMBER = 80;

    /**
     * This is the default maximum number of seconds to allow to elapse
     * beteen receiving one byte of a client request and
     * receiving the next byte, before timing out.
     */
    constexpr double DEFAULT_INACTIVITY_TIMEOUT_SECONDS = 1.0;

    /**
     * This is the default maximum number of seconds to allow to elapse
     * beteen receiving the first byte of a client request and
     * receiving the last byte of the request, before timing out.
     */
    constexpr double DEFAULT_REQUEST_TIMEOUT_SECONDS = 60.0;

    /**
     * This is the number of milliseconds to wait between rounds of polling
     * connections to check for timeouts.
     */
    constexpr unsigned int TIMER_POLLING_PERIOD_MILLISECONDS = 50;

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
     * These are the different results that can be indicated
     * when a string is parsed as a size integer.
     */
    enum class ParseSizeResult {
        /**
         * This indicates the size was parsed successfully.
         */
        Success,

        /**
         * This indicates the size had one or more characters
         * that were not digits.
         */
        NotANumber,

        /**
         * This indicates the size exceeded the maximum representable
         * size integer.
         */
        Overflow
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
    ParseSizeResult ParseSize(
        const std::string& numberString,
        size_t& number
    ) {
        number = 0;
        for (auto c: numberString) {
            if (
                (c < '0')
                || (c > '9')
            ) {
                return ParseSizeResult::NotANumber;
            }
            auto previousNumber = number;
            number *= 10;
            number += (uint16_t)(c - '0');
            if (
                (number / 10) != previousNumber
            ) {
                return ParseSizeResult::Overflow;
            }
        }
        return ParseSizeResult::Success;
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
        std::shared_ptr< Http::Request > request,
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
         * This is the time reported by the time keeper when
         * data was last received from the client.
         */
        double timeLastDataReceived = 0.0;

        /**
         * This is the time reported by the time keeper when
         * the current request was started.
         */
        double timeLastRequestStarted = 0.0;

        /**
         * This buffer is used to reassemble fragmented HTTP requests
         * received from the client.
         */
        std::string reassemblyBuffer;

        /**
         * This is the state of the next request, while it's still
         * being received and parsed.
         */
        std::shared_ptr< Http::Request > nextRequest = std::make_shared< Http::Request >();

        /**
         * This flag indicates whether or not the server is still
         * accepting requests from the client.
         */
        bool acceptingRequests = true;
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
         * This is the public port number to which clients may connect
         * to establish connections with this server.
         */
        uint16_t port = DEFAULT_PORT_NUMBER;

        /**
         * This is the maximum number of seconds to allow to elapse
         * beteen receiving one byte of a client request and
         * receiving the next byte, before timing out.
         */
        double inactivityTimeout = DEFAULT_INACTIVITY_TIMEOUT_SECONDS;

        /**
         * This is the maximum number of seconds to allow to elapse
         * beteen receiving the first byte of a client request and
         * receiving the last byte of the request, before timing out.
         */
        double requestTimeout = DEFAULT_REQUEST_TIMEOUT_SECONDS;

        /**
         * This flag indicates whether or not the server is running.
         */
        bool mobilized = false;

        /**
         * This is the transport layer currently bound.
         */
        std::shared_ptr< ServerTransport > transport;

        /**
         * This is the object used to track time in the server.
         */
        std::shared_ptr< TimeKeeper > timeKeeper;

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

        /**
         * This is a worker thread whose sole job is to monitor
         * open connections for two different situations:
         * 1.  Too much time elapsed between receiving two sequential
         *     bytes of a request.
         * 2.  Too much time elapsed between the start of a request
         *     and the receipt of the last byte of the request.
         * If either situation occurs, a "408 Request timeout" response
         * is given to the client, and then the connection is closed.
         */
        std::thread timer;

        /**
         * This flag indicates whether or not the timer thread should stop.
         */
        bool stopTimer = false;

        /**
         * This is used by the timer thread to wait on any
         * condition that it should cause it to wake up.
         */
        std::condition_variable timerWakeCondition;

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
         * This method is the body of the timer thread.
         * Until it's told to stop, it monitors connections
         * and closes them with a "408 Request Timeout"
         * if any timeouts occur.
         */
        void Timer() {
            std::unique_lock< decltype(mutex) > lock(mutex);
            while (!stopTimer) {
                const auto now = timeKeeper->GetCurrentTime();
                auto connections = activeConnections;
                for (const auto& connectionState: connections) {
                    if (
                        (now - connectionState->timeLastDataReceived > inactivityTimeout)
                        || (now - connectionState->timeLastRequestStarted > requestTimeout)
                    ) {
                        const auto response = std::make_shared< Response >();
                        response->statusCode = 408;
                        response->reasonPhrase = "Request Timeout";
                        response->headers.AddHeader("Connection", "close");
                        IssueResponse(connectionState, response);
                    }
                }
                (void)timerWakeCondition.wait_for(
                    lock,
                    std::chrono::milliseconds(TIMER_POLLING_PERIOD_MILLISECONDS),
                    [this]{ return stopTimer; }
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
                    switch (
                        ParseSize(
                            request->headers.GetHeaderValue("Content-Length"),
                            contentLength
                        )
                    ) {
                        case ParseSizeResult::NotANumber: {
                            request->state = Request::State::Error;
                        } return messageEnd;

                        case ParseSizeResult::Overflow: {
                            request->state = Request::State::Error;
                            request->responseStatusCode = 413;
                            request->responseReasonPhrase = "Payload Too Large";
                        } return messageEnd;
                    }
                    if (contentLength > MAX_CONTENT_LENGTH) {
                        request->state = Request::State::Error;
                        request->responseStatusCode = 413;
                        request->responseReasonPhrase = "Payload Too Large";
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
         * @param[in] connectionState
         *     This is the state of the connection for which to attempt
         *     to assemble the next request.
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
            StartNextRequest(connectionState);
            return request;
        }

        /**
         * This method prepares the connection for the next client request.
         *
         * @param[in] connectionState
         *     This is the state of the connection for which to prepare
         *     the next client request.
         */
        void StartNextRequest(
            std::shared_ptr< ConnectionState > connectionState
        ) {
            connectionState->nextRequest = std::make_shared< Request >();
            const auto now = timeKeeper->GetCurrentTime();
            connectionState->timeLastDataReceived = now;
            connectionState->timeLastRequestStarted = now;
        }

        /**
         * This method sends the given response back to the given client.
         *
         * @param[in] connectionState
         *     This is the state of the connection for which to issue
         *     the given response.
         *
         * @param[in] response
         *     This is the response to send back to the client.
         */
        void IssueResponse(
            std::shared_ptr< ConnectionState > connectionState,
            std::shared_ptr< Response > response
        ) {
            if (
                !response->headers.HasHeader("Transfer-Encoding")
                && !response->body.empty()
                && !response->headers.HasHeader("Content-Length")
            ) {
                response->headers.AddHeader(
                    "Content-Length",
                    SystemAbstractions::sprintf("%zu", response->body.length())
                );
            }
            const auto responseText = response->Generate();
            connectionState->connection->SendData(
                std::vector< uint8_t >(
                    responseText.begin(),
                    responseText.end()
                )
            );
            diagnosticsSender.SendDiagnosticInformationFormatted(
                1, "Sent %u '%s' response back to %s",
                response->statusCode,
                response->reasonPhrase.c_str(),
                connectionState->connection->GetPeerId().c_str()
            );
            bool closeRequested = false;
            for (const auto& connectionToken: response->headers.GetHeaderMultiValue("Connection")) {
                if (connectionToken == "close") {
                    closeRequested = true;
                    break;
                }
            }
            if (closeRequested) {
                connectionState->acceptingRequests = false;
                connectionState->connection->Break(true);
                OnConnectionBroken(
                    connectionState,
                    "closed by server"
                );
            }
        }

        /**
         * This method is called when a connection is broken,
         * either on the server end or the client end.
         *
         * @param[in] connectionState
         *     This is the state of the connection which is broken.
         *
         * @param[in] reason
         *     This describes how the connection was broken.
         */
        void OnConnectionBroken(
            std::shared_ptr< ConnectionState > connectionState,
            const std::string& reason
        ) {
            diagnosticsSender.SendDiagnosticInformationFormatted(
                2, "Connection to %s %s",
                connectionState->connection->GetPeerId().c_str(),
                reason.c_str()
            );
            (void)brokenConnections.insert(connectionState);
            reaperWakeCondition.notify_all();
            (void)activeConnections.erase(connectionState);
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
            if (!connectionState->acceptingRequests) {
                return;
            }
            const auto now = timeKeeper->GetCurrentTime();
            connectionState->timeLastDataReceived = now;
            connectionState->reassemblyBuffer += std::string(data.begin(), data.end());
            for (;;) {
                const auto request = TryRequestAssembly(connectionState);
                if (request == nullptr) {
                    break;
                }
                std::shared_ptr< Response > response;
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
                        response = resource->handler(request);
                    } else {
                        response = std::make_shared< Response >();
                        response->statusCode = 404;
                        response->reasonPhrase = "Not Found";
                        response->headers.SetHeader("Content-Type", "text/plain");
                        response->body = "FeelsBadMan\r\n";
                    }
                    const auto requestConnectionTokens = request->headers.GetHeaderMultiValue("Connection");
                    bool closeRequested = false;
                    for (const auto& connectionToken: requestConnectionTokens) {
                        if (connectionToken == "close") {
                            closeRequested = true;
                            break;
                        }
                    }
                    if (closeRequested) {
                        auto responseConnectionTokens = response->headers.GetHeaderMultiValue("Connection");
                        bool closeResponded = false;
                        for (const auto& connectionToken: responseConnectionTokens) {
                            if (connectionToken == "close") {
                                closeResponded = true;
                                break;
                            }
                        }
                        if (!closeResponded) {
                            responseConnectionTokens.push_back("close");
                        }
                        response->headers.SetHeader("Connection", responseConnectionTokens, true);
                    }
                } else {
                    response = std::make_shared< Response >();
                    response->statusCode = request->responseStatusCode;
                    response->reasonPhrase = request->responseReasonPhrase;
                    response->headers.SetHeader("Content-Type", "text/plain");
                    response->body = "FeelsBadMan\r\n";
                    if (request->state == Request::State::Error) {
                        response->headers.SetHeader("Connection", "close");
                    }
                }
                IssueResponse(connectionState, response);
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
            StartNextRequest(connectionState);
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
                    OnConnectionBroken(
                        connectionState,
                        "broken by peer"
                    );
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

    bool Server::Mobilize(const MobilizationDependencies& deps) {
        if (impl_->mobilized) {
            return false;
        }
        impl_->transport = deps.transport;
        if (
            impl_->transport->BindNetwork(
                impl_->port,
                [this](std::shared_ptr< Connection > connection){
                    impl_->NewConnection(connection);
                }
            )
        ) {
            impl_->diagnosticsSender.SendDiagnosticInformationFormatted(
                3, "Now listening on port %" PRIu16,
                impl_->port
            );
        } else {
            impl_->transport = nullptr;
            return false;
        }
        impl_->timeKeeper = deps.timeKeeper;
        impl_->stopTimer = false;
        impl_->timer = std::thread(&Impl::Timer, impl_.get());
        impl_->mobilized = true;
        return true;
    }

    void Server::Demobilize() {
        if (impl_->timer.joinable()) {
            {
                std::lock_guard< decltype(impl_->mutex) > lock(impl_->mutex);
                impl_->stopTimer = true;
                impl_->timerWakeCondition.notify_all();
            }
            impl_->timer.join();
        }
        if (impl_->transport != nullptr) {
            impl_->transport->ReleaseNetwork();
            impl_->transport = nullptr;
        }
        impl_->mobilized = false;
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
        } else if (key == "Port") {
            uint16_t newPort;
            if (
                sscanf(
                    value.c_str(),
                    "%" SCNu16,
                    &newPort
                ) == 1
            ) {
                impl_->diagnosticsSender.SendDiagnosticInformationFormatted(
                    0,
                    "Port number changed from %" PRIu16 " to %" PRIu16,
                    impl_->port,
                    newPort
                );
                impl_->port = newPort;
            }
        } else if (key == "InactivityTimeout") {
            double newInactivityTimeout;
            if (
                sscanf(
                    value.c_str(),
                    "%lf",
                    &newInactivityTimeout
                ) == 1
            ) {
                impl_->diagnosticsSender.SendDiagnosticInformationFormatted(
                    0,
                    "Inactivity timeout changed from %lf to %lf",
                    impl_->inactivityTimeout,
                    newInactivityTimeout
                );
                impl_->inactivityTimeout = newInactivityTimeout;
            }
        } else if (key == "RequestTimeout") {
            double newRequestTimeout;
            if (
                sscanf(
                    value.c_str(),
                    "%lf",
                    &newRequestTimeout
                ) == 1
            ) {
                impl_->diagnosticsSender.SendDiagnosticInformationFormatted(
                    0,
                    "Request timeout changed from %lf to %lf",
                    impl_->requestTimeout,
                    newRequestTimeout
                );
                impl_->requestTimeout = newRequestTimeout;
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

}
