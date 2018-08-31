/**
 * @file Client.cpp
 *
 * This module contains the implementation of the Http::Client class.
 *
 * © 2018 by Richard Walters
 */

#include <condition_variable>
#include <functional>
#include <Http/Client.hpp>
#include <Http/Connection.hpp>
#include <inttypes.h>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <sstream>
#include <SystemAbstractions/DiagnosticsSender.hpp>
#include <SystemAbstractions/StringExtensions.hpp>
#include <thread>

namespace {

    /**
     * This is the character sequence corresponding to a carriage return (CR)
     * followed by a line feed (LF), which officially delimits each
     * line of an HTTP response.
     */
    const std::string CRLF("\r\n");

    /**
     * This is the default port number associated with
     * the HTTP protocol and scheme.
     */
    constexpr uint16_t DEFAULT_HTTP_PORT_NUMBER = 80;

    /**
     * This is the number of milliseconds to wait between rounds of polling
     * connections to check for timeouts.
     */
    constexpr unsigned int CONNECTION_POLLING_PERIOD_MILLISECONDS = 50;

    /**
     * This method parses the protocol identifier, status code,
     * and reason phrase from the given status line.
     *
     * @param[in] response
     *     This is the response in which to store the parsed
     *     status code and reason phrase.
     *
     * @param[in] statusLine
     *     This is the raw status line string to parse.
     *
     * @return
     *     An indication of whether or not the status line
     *     was successfully parsed is returned.
     */
    bool ParseStatusLine(
        Http::Response& response,
        const std::string& statusLine
    ) {
        // Parse the protocol.
        const auto protocolDelimiter = statusLine.find(' ');
        if (protocolDelimiter == std::string::npos) {
            return false;
        }
        const auto protocol = statusLine.substr(0, protocolDelimiter);
        if (protocol != "HTTP/1.1") {
            return false;
        }

        // Parse the status code.
        const auto statusCodeDelimiter = statusLine.find(' ', protocolDelimiter + 1);
        if (statusCodeDelimiter == std::string::npos) {
            return false;
        }
        intmax_t statusCodeAsInt;
        if (
            SystemAbstractions::ToInteger(
                statusLine.substr(
                    protocolDelimiter + 1,
                    statusCodeDelimiter - protocolDelimiter - 1
                ),
                statusCodeAsInt
            ) != SystemAbstractions::ToIntegerResult::Success
        ) {
            return false;
        }
        if (statusCodeAsInt > 999) {
            return false;
        } else {
            response.statusCode = (unsigned int)statusCodeAsInt;
        }

        // Parse the reason phrase.
        response.reasonPhrase = statusLine.substr(statusCodeDelimiter + 1);
        return true;
    }

    /**
     * This method is called one or more times to incrementally parse
     * a raw HTTP response message.  For the first call, pass in a newly-
     * constructed response object, and the beginning of the raw
     * HTTP response message.  Contine calling with the same response
     * object and subsequent pieces of the raw HTTP response message,
     * until the response is fully parsed, as indicated by its
     * responseParsingState transitioning to ResponseParsingState::Complete,
     * ResponseParsingState::InvalidRecoverable, or
     * ResponseParsingState::InvalidUnrecoverable.
     *
     * @param[in,out] response
     *     This is the response being parsed.
     *
     * @param[in] nextRawResponsePart
     *     This is the next part of the raw HTTP response message.
     *
     * @return
     *     A count of the number of characters that were taken from
     *     the given input string is returned. Presumably,
     *     any characters past this point belong to another message or
     *     are outside the scope of HTTP.
     */
    size_t ParseResponseImpl(
        Http::Response& response,
        const std::string& nextRawResponsePart
    ) {
        // Count the number of characters incorporated into
        // the response object.
        size_t messageEnd = 0;

        // First, extract and parse the status line.
        if (response.state == Http::Response::State::StatusLine) {
            const auto statusLineEnd = nextRawResponsePart.find(CRLF);
            if (statusLineEnd == std::string::npos) {
                return messageEnd;
            }
            const auto statusLine = nextRawResponsePart.substr(0, statusLineEnd);
            messageEnd = statusLineEnd + CRLF.length();
            response.state = Http::Response::State::Headers;
            response.valid = ParseStatusLine(response, statusLine);
        }

        // Second, parse the message headers and identify where the body begins.
        if (response.state == Http::Response::State::Headers) {
            size_t bodyOffset;
            const auto headersState = response.headers.ParseRawMessage(
                nextRawResponsePart.substr(messageEnd),
                bodyOffset
            );
            messageEnd += bodyOffset;
            switch (headersState) {
                case MessageHeaders::MessageHeaders::State::Complete: {
                    if (!response.headers.IsValid()) {
                        response.valid = false;
                    }
                    response.state = Http::Response::State::Body;
                } break;

                case MessageHeaders::MessageHeaders::State::Incomplete: {
                } return messageEnd;

                case MessageHeaders::MessageHeaders::State::Error:
                default: {
                    response.state = Http::Response::State::Error;
                    return messageEnd;
                }
            }
        }

        // Finally, extract the body.
        if (response.state == Http::Response::State::Body) {
            // If there is a "Content-Length"
            // header, we carefully carve exactly that number of characters
            // out (and bail if we don't have enough).  Otherwise, we just
            // assume the body extends to the end of the raw message.
            const auto bytesAvailableForBody = nextRawResponsePart.length() - messageEnd;
            if (response.headers.HasHeader("Content-Length")) {
                intmax_t contentLengthAsInt;
                if (
                    SystemAbstractions::ToInteger(
                        response.headers.GetHeaderValue("Content-Length"),
                        contentLengthAsInt
                    ) != SystemAbstractions::ToIntegerResult::Success
                ) {
                    response.state = Http::Response::State::Error;
                    return messageEnd;
                }
                if (contentLengthAsInt < 0) {
                    response.state = Http::Response::State::Error;
                    return messageEnd;
                } else {
                    const auto contentLength = (size_t)contentLengthAsInt;
                    if (contentLength > bytesAvailableForBody) {
                        response.state = Http::Response::State::Body;
                        return messageEnd;
                    } else {
                        response.body = nextRawResponsePart.substr(messageEnd, contentLength);
                        messageEnd += contentLength;
                        response.state = Http::Response::State::Complete;
                    }
                }
            } else {
                response.body.clear();
                response.state = Http::Response::State::Complete;
            }
        }
        return messageEnd;
    }

    /**
     * This holds onto all the information that a client has about
     * a connection to a server.
     *
     * @note
     *     It would be better to name this ConnectionState, but this
     *     confuses the Visual Studio debugger, because the Server
     *     implementation has its own ConnectionState structure which
     *     is obviously different.
     */
    struct ClientConnectionState {
        /**
         * This is the connection to the server.
         */
        std::shared_ptr< Http::Connection > connection;

        /**
         * This refers to the current transaction (if any) that is
         * currently being made through the connection.
         */
        std::weak_ptr< struct TransactionImpl > currentTransaction;

        /**
         * This is the time at which the connection was established,
         * or the last data was received from the server, whichever
         * was most recent.
         */
        double lastReceiveTime = 0.0;

        /**
         * This is used to synchronize access to this object.
         */
        std::mutex mutex;
    };

    /**
     * This is a client transaction structure that contains the additional
     * properties and methods required by the client's implementation.
     */
    struct TransactionImpl
        : public Http::Client::Transaction
    {
        // Properties

        /**
         * This is the state of the connection to the server
         * used by this transaction.
         */
        std::shared_ptr< ClientConnectionState > connectionState;

        /**
         * This flag indicates whether or not the connection used to
         * communicate with the server should be kept open after the
         * request, possibly to be reused in subsequent requests.
         */
        bool persistConnection = false;

        /**
         * This flag indicates whether or not the transaction is complete.
         */
        bool complete = false;

        /**
         * This buffer is used to reassemble fragmented HTTP responses
         * received from the server.
         */
        std::string reassemblyBuffer;

        /**
         * This is used to synchronize access to the object.
         */
        std::recursive_mutex mutex;

        /**
         * This is used to wait for various object state changes.
         */
        std::condition_variable_any stateChange;

        // Methods

        /**
         * This method is called whenever the transaction is completed
         * and we aren't holding the object's mutex.
         */
        void Complete() {
            std::lock_guard< decltype(mutex) > lock(mutex);
            CompleteWithLock();
        }

        /**
         * This method is called whenever the transaction is completed
         * while we're holding the object's mutex.
         */
        void CompleteWithLock() {
            complete = true;
            const auto connection = connectionState->connection;
            connectionState = nullptr;
            if (
                (connection != nullptr)
                && !persistConnection
            ) {
                connection->Break(false);
            }
            stateChange.notify_all();
        }

        /**
         * This method is called when new data is received from the server.
         *
         * @param[in] data
         *     This is a copy of the data that was received from the server.
         */
        void DataReceived(const std::vector< uint8_t >& data) {
            std::lock_guard< decltype(mutex) > lock(mutex);
            reassemblyBuffer += std::string(data.begin(), data.end());
            const auto charactersAccepted = ParseResponseImpl(
                response,
                reassemblyBuffer
            );
            reassemblyBuffer.erase(
                reassemblyBuffer.begin(),
                reassemblyBuffer.begin() + charactersAccepted
            );
            if (response.IsCompleteOrError()) {
                state = Transaction::State::Completed;
                CompleteWithLock();
            }
        }

        // Http::Client::Transaction

        virtual bool AwaitCompletion(
            const std::chrono::milliseconds& relativeTime
        ) override {
            std::unique_lock< decltype(mutex) > lock(mutex);
            return stateChange.wait_for(
                lock,
                relativeTime,
                [this]{ return complete; }
            );
        };
    };

}

namespace Http {

    /**
     * This contains the private properties of a Client instance.
     */
    struct Client::Impl {
        // Properties

        /**
         * This is a helper object used to generate and publish
         * diagnostic messages.
         */
        SystemAbstractions::DiagnosticsSender diagnosticsSender;

        /**
         * This flag indicates whether or not the client is running.
         */
        bool mobilized = false;

        /**
         * This is the transport layer currently bound.
         */
        std::shared_ptr< ClientTransport > transport;

        /**
         * This is the object used to track time in the client.
         */
        std::shared_ptr< TimeKeeper > timeKeeper;

        /**
         * This is the amount of time after a request is made
         * of a server, before the transaction is considered timed out
         * if no part of a response has been received.
         */
        double requestTimeoutSeconds = DEFAULT_REQUEST_TIMEOUT_SECONDS;

        /**
         * This is used to hold onto persistent connections to servers.
         */
        std::map< std::string, std::shared_ptr< ClientConnectionState > > persistentConnections;

        /**
         * This is used to synchronize access to the object.
         */
        std::mutex mutex;

        /**
         * This is used to wake up the worker thread.
         */
        std::condition_variable workerWakeCondition;

        /**
         * This flag indicates whether or not the worker thread should stop.
         */
        bool stopWorker = false;

        /**
         * This thread performs background housekeeping for the client, such as:
         * - Drop any persistent connections which have broken.
         * - Complete any transactions that have timed out.
         */
        std::thread worker;

        // Lifecycle management

        ~Impl() noexcept {
            Demobilize();
        }
        Impl(const Impl&) = delete;
        Impl(Impl&&) noexcept = delete;
        Impl& operator=(const Impl&) = delete;
        Impl& operator=(Impl&&) noexcept = delete;

        // Methods

        /**
         * This is the constructor for the structure.
         */
        Impl()
            : diagnosticsSender("Http::Client")
        {
        }

        /**
         * This method should be called when the client is mobilized,
         * in order to start background housekeeping.
         */
        void Mobilize() {
            if (!worker.joinable()) {
                stopWorker = false;
                worker = std::thread(&Impl::Worker, this);
            }
        }

        /**
         * This method should be called when the client is demobilized,
         * in order to stop background housekeeping.
         */
        void Demobilize() {
            if (worker.joinable()) {
                {
                    std::lock_guard< decltype(mutex) > lock(mutex);
                    stopWorker = true;
                    workerWakeCondition.notify_all();
                }
                worker.join();
            }
        }

        /**
         * This thread performs background housekeeping for the client, such as:
         * - Drop any persistent connections which have broken.
         * - Complete any transactions that have timed out.
         */
        void Worker() {
            std::unique_lock< decltype(mutex) > lock(mutex);
            while (!stopWorker) {
                (void)workerWakeCondition.wait_for(
                    lock,
                    std::chrono::milliseconds(CONNECTION_POLLING_PERIOD_MILLISECONDS),
                    [this]{ return stopWorker; }
                );
                const auto now = timeKeeper->GetCurrentTime();
                std::set< std::shared_ptr< ClientConnectionState > > timedOutConnectionStates;
                for (
                    auto persistentConnectionsEntry = persistentConnections.begin();
                    persistentConnectionsEntry != persistentConnections.end();
                ) {
                    auto persistentConnection = persistentConnectionsEntry->second;
                    if (now - persistentConnection->lastReceiveTime >= requestTimeoutSeconds) {
                        (void)timedOutConnectionStates.insert(persistentConnection);
                        persistentConnectionsEntry = persistentConnections.erase(persistentConnectionsEntry);
                    } else {
                        ++persistentConnectionsEntry;
                    }
                }
                {
                    lock.unlock();
                    for (auto connectionState: timedOutConnectionStates) {
                        std::shared_ptr< TransactionImpl > transaction;
                        {
                            std::lock_guard< decltype(connectionState->mutex) > lock(connectionState->mutex);
                            transaction = connectionState->currentTransaction.lock();
                        }
                        if (transaction == nullptr) {
                            return;
                        }
                        transaction->state = Transaction::State::Timeout;
                        transaction->Complete();
                    }
                    timedOutConnectionStates.clear();
                    lock.lock();
                }
            }
        }
    };

    Client::~Client() {
        Demobilize();
    }

    Client::Client()
        : impl_(new Impl)
    {
    }

    SystemAbstractions::DiagnosticsSender::UnsubscribeDelegate Client::SubscribeToDiagnostics(
        SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate delegate,
        size_t minLevel
    ) {
        return impl_->diagnosticsSender.SubscribeToDiagnostics(delegate, minLevel);
    }

    void Client::Mobilize(const MobilizationDependencies& deps) {
        if (impl_->mobilized) {
            return;
        }
        impl_->transport = deps.transport;
        impl_->timeKeeper = deps.timeKeeper;
        impl_->requestTimeoutSeconds = deps.requestTimeoutSeconds;
        impl_->mobilized = true;
        impl_->Mobilize();
    }

    auto Client::Request(
        Http::Request request,
        bool persistConnection
    ) -> std::shared_ptr< Transaction > {
        const auto transaction = std::make_shared< TransactionImpl >();
        const auto& hostNameOrAddress = request.target.GetHost();
        auto port = DEFAULT_HTTP_PORT_NUMBER;
        if (request.target.HasPort()) {
            port = request.target.GetPort();
        }
        const auto serverId = SystemAbstractions::sprintf(
            "%s:%" PRIu16,
            hostNameOrAddress.c_str(),
            port
        );
        std::shared_ptr< ClientConnectionState > connectionState;
        {
            std::lock_guard< decltype(impl_->mutex) > lock(impl_->mutex);
            const auto persistentConnectionsEntry = impl_->persistentConnections.find(serverId);
            if (persistentConnectionsEntry == impl_->persistentConnections.end()) {
                connectionState = std::make_shared< ClientConnectionState >();
                connectionState->lastReceiveTime = impl_->timeKeeper->GetCurrentTime();
                std::weak_ptr< ClientConnectionState > connectionStateWeak(connectionState);
                const auto brokenDelegate = [this, connectionStateWeak, serverId]{
                    const auto connectionState = connectionStateWeak.lock();
                    if (connectionState == nullptr) {
                        return;
                    }
                    std::lock_guard< decltype(impl_->mutex) > lock(impl_->mutex);
                    (void)impl_->persistentConnections.erase(serverId);
                };
                auto timeKeeper = impl_->timeKeeper;
                connectionState->connection = impl_->transport->Connect(
                    hostNameOrAddress,
                    port,
                    [connectionStateWeak, timeKeeper](const std::vector< uint8_t >& data){
                        const auto connectionState = connectionStateWeak.lock();
                        if (connectionState == nullptr) {
                            return;
                        }
                        connectionState->lastReceiveTime = timeKeeper->GetCurrentTime();
                        std::shared_ptr< TransactionImpl > transaction;
                        {
                            std::lock_guard< decltype(connectionState->mutex) > lock(connectionState->mutex);
                            transaction = connectionState->currentTransaction.lock();
                        }
                        if (transaction == nullptr) {
                            return;
                        }
                        if (transaction->connectionState == nullptr) {
                            return;
                        }
                        transaction->DataReceived(data);
                    },
                    [connectionStateWeak, brokenDelegate](bool){
                        const auto connectionState = connectionStateWeak.lock();
                        if (connectionState == nullptr) {
                            return;
                        }
                        std::shared_ptr< TransactionImpl > transaction;
                        {
                            std::lock_guard< decltype(connectionState->mutex) > lock(connectionState->mutex);
                            transaction = connectionState->currentTransaction.lock();
                        }
                        if (transaction == nullptr) {
                            return;
                        }
                        if (transaction->connectionState == nullptr) {
                            return;
                        }
                        transaction->state = Transaction::State::Broken;
                        transaction->Complete();
                        brokenDelegate();
                    }
                );
            } else {
                connectionState = persistentConnectionsEntry->second;
            }
        }
        {
            std::lock_guard< decltype(connectionState->mutex) > lock(connectionState->mutex);
            connectionState->currentTransaction = transaction;
        }
        transaction->connectionState = connectionState;
        if (connectionState->connection == nullptr) {
            transaction->state = Transaction::State::UnableToConnect;
            transaction->Complete();
            return transaction;
        }
        transaction->persistConnection = persistConnection;
        request.headers.SetHeader("Host", hostNameOrAddress);
        if (!persistConnection) {
            request.headers.SetHeader("Connection", "Close");
        }
        const auto requestEncoding = request.Generate();
        connectionState->connection->SendData({requestEncoding.begin(), requestEncoding.end()});
        {
            std::lock_guard< decltype(impl_->mutex) > lock(impl_->mutex);
            if (persistConnection) {
                impl_->persistentConnections[serverId] = connectionState;
            } else {
                (void)impl_->persistentConnections.erase(serverId);
            }
        }
        if (!persistConnection) {
            connectionState->connection->Break(true);
        }
        return transaction;
    }

    void Client::Demobilize() {
        impl_->Demobilize();
        impl_->timeKeeper = nullptr;
        impl_->transport = nullptr;
        impl_->mobilized = false;
    }

    auto Client::ParseResponse(const std::string& rawResponse) -> std::shared_ptr< Response > {
        size_t messageEnd;
        return ParseResponse(rawResponse, messageEnd);
    }

    auto Client::ParseResponse(
        const std::string& rawResponse,
        size_t& messageEnd
    ) -> std::shared_ptr< Response > {
        const auto response = std::make_shared< Response >();
        messageEnd = ParseResponseImpl(*response, rawResponse);
        if (response->IsCompleteOrError()) {
            return response;
        } else {
            return nullptr;
        }
    }

    void PrintTo(
        const Http::Client::Transaction::State& state,
        std::ostream* os
    ) {
        switch (state) {
            case Http::Client::Transaction::State::InProgress: {
                *os << "In Progress";
            } break;
            case Http::Client::Transaction::State::Completed: {
                *os << "Completed";
            } break;
            case Http::Client::Transaction::State::UnableToConnect: {
                *os << "UNABLE TO CONNECT";
            } break;
            case Http::Client::Transaction::State::Broken: {
                *os << "BROKEN";
            } break;
            case Http::Client::Transaction::State::Timeout: {
                *os << "TIMED OUT";
            } break;
            default: {
                *os << "???";
            };
        }
    }

}
