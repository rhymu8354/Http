/**
 * @file Client.cpp
 *
 * This module contains the implementation of the Http::Client class.
 *
 * © 2018 by Richard Walters
 */

#include "Inflate.hpp"

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <Http/ChunkedBody.hpp>
#include <Http/Client.hpp>
#include <Http/Connection.hpp>
#include <inttypes.h>
#include <limits>
#include <list>
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
     * These are the names of message headers which aren't allowed
     * to be transmitted in the trailer of a chunked body of a message,
     * as listed in section 4.1.2 of RFC 7230.
     */
    std::set< MessageHeaders::MessageHeaders::HeaderName > HEADERS_NOT_ALLOWED_IN_TRAILER{
        // Message framing
        "Transfer-Encoding",
        "Content-Length",

        // Host
        "Host",

        // Request modifiers: controls
        "Cache-Control",
        "Expect",
        //"Host",  // explicitly listed earlier
        "Max-Forwards",
        "Pragma",
        "Range",
        "TE",

        // Request modifiers: authentication
        "Authorization",
        "Proxy-Authenticate",
        "Proxy-Authorization",
        "WWW-Authenticate",

        // Request modifiers: cookies
        "Cookie",
        "Set-Cookie",
        "Cookie2",
        "Set-Cookie2",

        // Response control data
        "Age",
        "Cache-Control",
        "Expires",
        "Date",
        "Location",
        "Retry-After",
        "Vary",
        "Warning",

        // Payload processing
        "Content-Encoding",
        "Content-Type",
        "Content-Range",
        "Trailer",
    };

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
     * @param[in,out] chunkedBody
     *     This is used to incrementally parse the response body,
     *     if the response employs chunked transfer coding.
     *
     * @param[in] nextRawResponsePart
     *     This is the next part of the raw HTTP response message.
     *
     * @param[in] decodeSupportedCodings
     *     This flag indicates whether or not the function decodes
     *     the body if the coding applied to it is understood by this
     *     function.
     *
     * @return
     *     A count of the number of characters that were taken from
     *     the given input string is returned. Presumably,
     *     any characters past this point belong to another message or
     *     are outside the scope of HTTP.
     */
    size_t ParseResponseImpl(
        Http::Response& response,
        Http::ChunkedBody& chunkedBody,
        const std::string& nextRawResponsePart,
        bool decodeSupportedCodings
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

        // Next, extract the body.
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
            } else if (response.headers.HasHeaderToken("Transfer-Encoding", "chunked")) {
                messageEnd += chunkedBody.Decode(nextRawResponsePart, messageEnd);
                switch (chunkedBody.GetState()) {
                    case Http::ChunkedBody::State::Complete: {
                        response.body = chunkedBody;
                        for (const auto& trailer: chunkedBody.GetTrailers().GetAll()) {
                            if (HEADERS_NOT_ALLOWED_IN_TRAILER.find(trailer.name) == HEADERS_NOT_ALLOWED_IN_TRAILER.end()) {
                                response.headers.AddHeader(trailer.name, trailer.value);
                            }
                        }
                        response.headers.SetHeader(
                            "Content-Length",
                            SystemAbstractions::sprintf("%zu", response.body.length())
                        );
                        auto transferCodings = response.headers.GetHeaderTokens("Transfer-Encoding");
                        const auto chunkedToken = std::find(
                            transferCodings.begin(),
                            transferCodings.end(),
                            "chunked"
                        );
                        transferCodings.erase(chunkedToken);
                        if (transferCodings.empty()) {
                            response.headers.RemoveHeader("Transfer-Encoding");
                        } else {
                            response.headers.SetHeader(
                                "Transfer-Encoding",
                                SystemAbstractions::Join(transferCodings, " ")
                            );
                        }
                        response.headers.RemoveHeader("Trailer");
                        response.state = Http::Response::State::Complete;
                    } break;

                    case Http::ChunkedBody::State::Error: {
                        response.state = Http::Response::State::Error;
                    } break;

                    default: {
                    } break;
                }
            } else if (response.headers.HasHeaderToken("Connection", "close")) {
                response.body += nextRawResponsePart.substr(messageEnd);
                messageEnd += bytesAvailableForBody;
            } else {
                response.body.clear();
                response.state = Http::Response::State::Complete;
            }
        }

        // Finally, decode the body if there are any content encodings
        // applied that we should handle.
        if (
            (response.state == Http::Response::State::Complete)
            && decodeSupportedCodings
        ) {
            auto codings = response.headers.GetHeaderTokens("Content-Encoding");
            std::reverse(codings.begin(), codings.end());
            std::list< std::string > codingsNotApplied;
            bool stopDecoding = false;
            for (const auto& coding: codings) {
                if (stopDecoding) {
                    codingsNotApplied.push_front(coding);
                } else {
                    static const std::map< std::string, Http::InflateMode > inflateModesSupported{
                        {"gzip", Http::InflateMode::Ungzip},
                        {"deflate", Http::InflateMode::Inflate},
                    };
                    const auto codingEntry = inflateModesSupported.find(coding);
                    if (codingEntry == inflateModesSupported.end()) {
                        stopDecoding = true;
                        codingsNotApplied.push_front(coding);
                    } else {
                        std::string decodedBody;
                        if (Http::Inflate(response.body, decodedBody, codingEntry->second)) {
                            response.body = std::move(decodedBody);
                            response.headers.SetHeader(
                                "Content-Length",
                                SystemAbstractions::sprintf("%zu", response.body.size())
                            );
                        } else {
                            response.state = Http::Response::State::Error;
                            break;
                        }
                    }
                }
            }
            std::string codingsNotAppliedString;
            for (const auto& coding: codings) {
                if (!codingsNotAppliedString.empty()) {
                    codingsNotAppliedString += ", ";
                }
                codingsNotAppliedString += coding;
            }
            if (codingsNotApplied.empty()) {
                response.headers.RemoveHeader("Content-Encoding");
            } else {
                response.headers.SetHeader("Content-Encoding", codingsNotAppliedString);
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
         * This is the time at which the connection was last used for a
         * transaction.
         */
        double lastTransactionTime = 0.0;

        /**
         * This flag indicates whether or not the connection to the server has
         * been broken.
         */
        bool broken = false;

        /**
         * This is used to synchronize access to this object.
         */
        std::recursive_mutex mutex;
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
         * This is the time at which the transaction was started,
         * or the last data was received for the transaction, whichever
         * was most recent.
         */
        double lastReceiveTime = 0.0;

        /**
         * This is a utility used to decode the body of the response
         * obtained from the server, if the response employs
         * chunked transfer coding.
         */
        Http::ChunkedBody chunkedBody;

        /**
         * This is the state of the connection to the server
         * used by this transaction.
         */
        std::shared_ptr< ClientConnectionState > connectionState;

        /**
         * This is the connection upgrade delegate provided by the user,
         * to be called if the server upgrades the connection in the
         * response to the request.
         */
        Http::Client::UpgradeDelegate upgradeDelegate;

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

        /**
         * If set, this is a function to call once the transaction is
         * completed.
         */
        std::function< void() > completionDelegate;

        // Methods

        /**
         * This method is called whenever the transaction is completed
         * while we're holding the object's mutex.  The call has no
         * effect if the transaction is already complete.
         *
         * @param[in] endState
         *     This is the state to which to transition the transaction
         *     if this call causes it to be completed.
         *
         * @param[in] now
         *     This is the current time.
         *
         * @return
         *     An indication of whether or not the connection should be
         *     dropped by the client is returned.
         */
        bool Complete(
            Transaction::State endState,
            double now
        ) {
            std::unique_lock< decltype(mutex) > lock(mutex);
            if (complete) {
                return false;
            }
            bool dropConnection = false;
            if (
                (endState == State::Completed)
                && (response.statusCode == 101)
            ) {
                dropConnection = true;
                if (upgradeDelegate != nullptr) {
                    lock.unlock();
                    dropConnection = true;
                    upgradeDelegate(
                        response,
                        connectionState->connection,
                        reassemblyBuffer
                    );
                    lock.lock();
                }
            }
            complete = true;
            state = endState;
            {
                std::lock_guard< decltype(connectionState->mutex) > lock(connectionState->mutex);
                if (
                    (connectionState->connection != nullptr)
                    && (
                        !persistConnection
                        || (state == State::Timeout)
                    )
                ) {
                    connectionState->connection->Break(false);
                }
                connectionState->currentTransaction.reset();
                connectionState->lastTransactionTime = now;
            }
            stateChange.notify_all();
            auto completionDelegateCopy = completionDelegate;
            lock.unlock();
            if (completionDelegateCopy != nullptr) {
                completionDelegateCopy();
            }
            return dropConnection;
        }

        /**
         * This method is called when new data is received from the server.
         *
         * @param[in] data
         *     This is a copy of the data that was received from the server.
         *
         * @param[in] now
         *     This is the current time.
         *
         * @return
         *     An indication of whether or not the connection should be
         *     dropped by the client is returned.
         */
        bool DataReceived(
            const std::vector< uint8_t >& data,
            double now
        ) {
            std::unique_lock< decltype(mutex) > lock(mutex);
            if (complete) {
                return false;
            }
            lastReceiveTime = now;
            reassemblyBuffer += std::string(data.begin(), data.end());
            const auto charactersAccepted = ParseResponseImpl(
                response,
                chunkedBody,
                reassemblyBuffer,
                true
            );
            reassemblyBuffer.erase(
                reassemblyBuffer.begin(),
                reassemblyBuffer.begin() + charactersAccepted
            );
            lock.unlock();
            if (response.IsCompleteOrError()) {
                return Complete(State::Completed, now);
            }
            return false;
        }

        /**
         * This method is called if the connection to the server is broken.
         *
         * @param[in] now
         *     This is the current time.
         */
        void ConnectionBroken(double now) {
            std::unique_lock< decltype(mutex) > lock(mutex);
            if (complete) {
                return;
            }
            State endState;
            if (response.IsCompleteOrError(false)) {
                response.headers.SetHeader(
                    "Content-Length",
                    SystemAbstractions::sprintf("%zu", response.body.length())
                );
                endState = State::Completed;
            } else {
                endState = State::Broken;
            }
            lock.unlock();
            (void)Complete(endState, now);
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

        virtual void SetCompletionDelegate(
            std::function< void() > completionDelegate
        ) override {
            std::unique_lock< decltype(mutex) > lock(mutex);
            this->completionDelegate = completionDelegate;
            const bool wasComplete = complete;
            lock.unlock();
            if (wasComplete) {
                completionDelegate();
            }
        }

    };

    /**
     * This represents the pool of persistent connections available to a Client
     * instance.
     */
    class ClientConnectionPool {
        // Types
    public:
        /**
         * This is the type used to hold  a set of related client connections.
         */
        typedef std::set< std::shared_ptr< ClientConnectionState > > ConnectionSet;

        // Public Methods
    public:
        /**
         * This method marks as broken and removes from the connection pool any
         * connections whose last transaction time is before the given cutoff
         * time.
         *
         * @param[in] cutoff
         *     This is the time to compare to the last transaction times of
         *     connections to select the connections to drop.
         *
         * @return
         *     The set of connections removed from the connection pool
         *     is returned.
         */
        std::set< std::shared_ptr< ClientConnectionState > > DropInactive(double cutoff) {
            std::lock_guard< decltype(mutex) > poolLock(mutex);
            std::set< std::shared_ptr< ClientConnectionState > > inactiveConnections;
            for (
                auto connectionsEntry = connections.begin();
                connectionsEntry != connections.end();
            ) {
                for (
                    auto connection = connectionsEntry->second.begin();
                    connection != connectionsEntry->second.end();
                ) {
                    std::lock_guard< decltype((*connection)->mutex) > connectionLock((*connection)->mutex);
                    if (
                        ((*connection)->currentTransaction.lock() == nullptr)
                        && ((*connection)->lastTransactionTime <= cutoff)
                    ) {
                        (*connection)->broken = true;
                        (void)inactiveConnections.insert(*connection);
                        connection = connectionsEntry->second.erase(connection);
                    } else {
                        ++connection;
                    }
                }
                if (connectionsEntry->second.empty()) {
                    connectionsEntry = connections.erase(connectionsEntry);
                } else {
                    ++connectionsEntry;
                }
            }
            return inactiveConnections;
        }

        /**
         * This method attempts to find a connection in the pool that isn't
         * broken and doesn't have any transaction using it.  If one is found,
         * the given transaction is assigned to it, and the connection is
         * returned.  Otherwise, nullptr is returned.
         *
         * @param[in] serverId
         *     This is a unique identifier of the server for which a connection
         *     is needed, in the form "host:port".
         *
         * @param[in] transaction
         *     This is the transaction to attach to a free connection,
         *     if found.
         *
         * @param[in] transactionTime
         *     This is the time at which the transaction began.
         *
         * @return
         *     If the given transaction was successfully assigned to a
         *     connection, the connection is returned.
         *
         * @retval nullptr
         *     This is returned if no free connection could be found for
         *     the transaction.
         */
        std::shared_ptr< ClientConnectionState > AttachTransaction(
            const std::string& serverId,
            std::shared_ptr< TransactionImpl > transaction,
            double transactionTime
        ) {
            std::lock_guard< decltype(mutex) > poolLock(mutex);
            const auto connectionsEntry = connections.find(serverId);
            if (connectionsEntry != connections.end()) {
                for (const auto& connection: connectionsEntry->second) {
                    std::lock_guard< decltype(connection->mutex) > connectionLock(connection->mutex);
                    if (
                        !connection->broken
                        && (connection->currentTransaction.lock() == nullptr)
                    ) {
                        connection->currentTransaction = transaction;
                        connection->lastTransactionTime = transactionTime;
                        return connection;
                    }
                }
            }
            return nullptr;
        }

        /**
         * This method adds the given connection to the pool, if it isn't
         * broken.
         *
         * @param[in] serverId
         *     This is a unique identifier of the server end of the connection,
         *     in the form "host:port".
         *
         * @param[in] connection
         *     This is the connection to add to the pool.
         */
        void AddConnection(
            const std::string& serverId,
            std::shared_ptr< ClientConnectionState > connection
        ) {
            std::lock_guard< decltype(mutex) > poolLock(mutex);
            std::lock_guard< decltype(connection->mutex) > connectionLock(connection->mutex);
            if (!connection->broken) {
                (void)connections[serverId].insert(connection);
            }
        }

        /**
         * This method removes the given connection from the pool.
         *
         * @param[in] serverId
         *     This is a unique identifier of the server end of the connection,
         *     in the form "host:port".
         *
         * @param[in] connection
         *     This is the connection to remove from the pool.
         */
        void DropConnection(
            const std::string& serverId,
            std::shared_ptr< ClientConnectionState > connection
        ) {
            std::lock_guard< decltype(mutex) > poolLock(mutex);
            auto& connectionPool = connections[serverId];
            (void)connectionPool.erase(connection);
            if (connectionPool.empty()) {
                (void)connections.erase(serverId);
            }
        }

        // Private Properties
    private:
        /**
         * This is used to synchronize access to the object.
         */
        std::mutex mutex;

        /**
         * These are the connections in the pool, organized by server
         * identifier (host:port).
         */
        std::map< std::string, ConnectionSet > connections;
    };

}

namespace Http {

    /**
     * This contains the private properties of a Client instance.
     */
    struct Client::Impl {
        // Types

        /**
         * This is the type used to handle collections of transactions that the
         * client knows about.  The keys are arbitrary (but unique for the
         * client instance) identifiers used to help select transactions from
         * collections.
         */
        typedef std::map< unsigned int, std::weak_ptr< TransactionImpl > > TransactionCollection;

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
         * This is the amount of time, after a transaction is completed,
         * that a persistent connection is closed if another transaction
         * does not reuse the connection.
         */
        double inactivityInterval = DEFAULT_INACTIVITY_INTERVAL_SECONDS;

        /**
         * This is used to hold onto persistent connections to servers.
         */
        std::shared_ptr< ClientConnectionPool > persistentConnections = std::make_shared< ClientConnectionPool >();

        /**
         * This is the collection of client transactions currently active,
         * keyed by transaction ID.
         */
        TransactionCollection activeTransactions;

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
         * This is the ID to assign to the next transaction.
         */
        unsigned int nextTransactionId = 1;

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
         * This method attempts to make a new connection to a given server, for
         * use by the given transaction.
         *
         * @param[in] transaction
         *     This is the transaction which needs a connection to the server.
         *
         * @param[in] scheme
         *     This is the scheme indicated in the URI of the target
         *     to which to establish a connection.
         *
         * @param[in] serverId
         *     This is a unique identifier of the server for which a connection
         *     is needed, in the form "host:port".
         *
         * @param[in] hostNameOrAddress
         *     This is the host name, or IP address in string form, of the
         *     server to which to connect.
         *
         * @param[in] port
         *     This is the TCP port number of the server to which to connect.
         *
         * @return
         *     A new connection state object representing the connection is
         *     returned, with the given transaction assigned to it.  If the
         *     connection failed, the connection object within the connection
         *     state object will be nullptr.
         */
        std::shared_ptr< ClientConnectionState > NewConnection(
            std::shared_ptr< TransactionImpl > transaction,
            const std::string& scheme,
            const std::string& serverId,
            const std::string& hostNameOrAddress,
            uint16_t port
        ) {
            const auto connectionState = std::make_shared< ClientConnectionState >();
            std::lock_guard< decltype(connectionState->mutex) > lock(connectionState->mutex);
            std::weak_ptr< ClientConnectionState > connectionStateWeak(connectionState);
            std::weak_ptr< ClientConnectionPool > persistentConnectionsWeak(persistentConnections);
            auto timeKeeperRef = timeKeeper;
            connectionState->connection = transport->Connect(
                scheme,
                hostNameOrAddress,
                port,
                [
                    connectionStateWeak,
                    serverId,
                    persistentConnectionsWeak,
                    timeKeeperRef
                ](const std::vector< uint8_t >& data){
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
                    if (
                        transaction->DataReceived(
                            data,
                            timeKeeperRef->GetCurrentTime()
                        )
                    ) {
                        const auto persistentConnections = persistentConnectionsWeak.lock();
                        if (persistentConnections != nullptr) {
                            persistentConnections->DropConnection(serverId, connectionState);
                        }
                    }
                },
                [
                    connectionStateWeak,
                    serverId,
                    persistentConnectionsWeak,
                    timeKeeperRef
                ](bool){
                    const auto connectionState = connectionStateWeak.lock();
                    if (connectionState == nullptr) {
                        return;
                    }
                    std::shared_ptr< TransactionImpl > transaction;
                    {
                        std::lock_guard< decltype(connectionState->mutex) > lock(connectionState->mutex);
                        transaction = connectionState->currentTransaction.lock();
                        connectionState->broken = true;
                    }
                    if (transaction == nullptr) {
                        return;
                    }
                    transaction->ConnectionBroken(timeKeeperRef->GetCurrentTime());
                    const auto persistentConnections = persistentConnectionsWeak.lock();
                    if (persistentConnections != nullptr) {
                        persistentConnections->DropConnection(serverId, connectionState);
                    }
                }
            );
            connectionState->currentTransaction = transaction;
            connectionState->lastTransactionTime = timeKeeper->GetCurrentTime();
            return connectionState;
        }

        /**
         * This method adds the given transaction to the collection of
         * transactions active for the client.
         *
         * @param[in] transaction
         *     This is the transaction to add to the collection of transactions
         *     active for the client.
         */
        void AddTransaction(std::shared_ptr< TransactionImpl > transaction) {
            std::lock_guard< decltype(mutex) > lock(mutex);
            activeTransactions[nextTransactionId++] = transaction;
        }

        /**
         * This method checks all the transactions in the given set to
         * see if any are completed or should be completed due to timeout.
         * Any transactions that should be completed due to timeout are
         * completed.
         *
         * @param[in] transactions
         *     These are the transactions to check.
         *
         * @param[in] cutoff
         *     This is the time to compare to the last receive times of
         *     transactions to select the transactions that should be
         *     timed out.
         *
         * @return
         *     The IDs of transactions which have been completed are returned.
         */
        static std::set< unsigned int > CheckTransactions(
            const TransactionCollection& transactions,
            double cutoff
        ) {
            std::set< unsigned int > completedTransactions;
            for (const auto& transactionsEntry: transactions) {
                bool isCompleted = false;
                auto transaction = transactionsEntry.second.lock();
                if (transaction == nullptr) {
                    isCompleted = true;
                } else {
                    std::unique_lock< decltype(transaction->mutex) > lock(transaction->mutex);
                    if (transaction->complete) {
                        isCompleted = true;
                    } else if (transaction->lastReceiveTime <= cutoff) {
                        lock.unlock();
                        (void)transaction->Complete(
                            Transaction::State::Timeout,
                            transaction->lastReceiveTime
                        );
                        isCompleted = true;
                    }
                }
                if (isCompleted) {
                    (void)completedTransactions.insert(transactionsEntry.first);
                }
            }
            return completedTransactions;
        }

        /**
         * This thread performs background housekeeping for the client, such as:
         * - Drop any persistent connections which have broken.
         * - Complete any transactions that have timed out.
         */
        void Worker() {
            std::unique_lock< decltype(mutex) > lock(mutex);
            while (!stopWorker) {
                // Wait one polling period.
                (void)workerWakeCondition.wait_for(
                    lock,
                    std::chrono::milliseconds(CONNECTION_POLLING_PERIOD_MILLISECONDS),
                    [this]{ return stopWorker; }
                );

                // Check for completed or timed-out transactions, and remove
                // any that have completed.
                TransactionCollection activeTransactionsCopy(activeTransactions);
                lock.unlock();
                auto completedTransactionIds = CheckTransactions(
                    activeTransactionsCopy,
                    timeKeeper->GetCurrentTime() - requestTimeoutSeconds
                );
                lock.lock();
                for (const auto completedTransactionId: completedTransactionIds) {
                    (void)activeTransactions.erase(completedTransactionId);
                }

                // Check for inactive persistent connections.
                const auto droppedConnections = persistentConnections->DropInactive(
                    timeKeeper->GetCurrentTime() - inactivityInterval
                );
            }
        }
    };

    Client::~Client() noexcept {
        Demobilize();
    }

    Client::Client()
        : impl_(new Impl)
    {
    }

    void Client::Mobilize(const MobilizationDependencies& deps) {
        if (impl_->mobilized) {
            return;
        }
        impl_->transport = deps.transport;
        impl_->timeKeeper = deps.timeKeeper;
        impl_->requestTimeoutSeconds = deps.requestTimeoutSeconds;
        impl_->inactivityInterval = deps.inactivityInterval;
        impl_->mobilized = true;
        impl_->Mobilize();
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
        ChunkedBody chunkedBody;
        messageEnd = ParseResponseImpl(*response, chunkedBody, rawResponse, false);
        if (response->IsCompleteOrError(false)) {
            return response;
        } else {
            return nullptr;
        }
    }

    SystemAbstractions::DiagnosticsSender::UnsubscribeDelegate Client::SubscribeToDiagnostics(
        SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate delegate,
        size_t minLevel
    ) {
        return impl_->diagnosticsSender.SubscribeToDiagnostics(delegate, minLevel);
    }

    auto Client::Request(
        Http::Request request,
        bool persistConnection,
        UpgradeDelegate upgradeDelegate
    ) -> std::shared_ptr< Transaction > {
        const auto transaction = std::make_shared< TransactionImpl >();
        transaction->upgradeDelegate = upgradeDelegate;
        if (upgradeDelegate != nullptr) {
            persistConnection = true;
        }
        transaction->lastReceiveTime = impl_->timeKeeper->GetCurrentTime();
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
        const auto now = impl_->timeKeeper->GetCurrentTime();
        auto connectionState = impl_->persistentConnections->AttachTransaction(
            serverId,
            transaction,
            now
        );
        if (connectionState == nullptr) {
            connectionState = impl_->NewConnection(
                transaction,
                request.target.GetScheme(),
                serverId,
                hostNameOrAddress,
                port
            );
        }
        if (
            (connectionState->connection != nullptr)
            && persistConnection
        ) {
            impl_->persistentConnections->AddConnection(serverId, connectionState);
        } else {
            impl_->persistentConnections->DropConnection(serverId, connectionState);
        }
        transaction->connectionState = connectionState;
        if (connectionState->connection == nullptr) {
            (void)transaction->Complete(
                Transaction::State::UnableToConnect,
                now
            );
            return transaction;
        }
        transaction->persistConnection = persistConnection;
        request.headers.SetHeader("Host", hostNameOrAddress);
        request.headers.SetHeader("Accept-Encoding", "gzip, deflate");
        if (!persistConnection) {
            request.headers.SetHeader("Connection", "Close");
        }
        const auto originalTarget = request.target;
        request.target = Uri::Uri();
        request.target.SetPath(originalTarget.GetPath());
        if (originalTarget.HasQuery()) {
            request.target.SetQuery(originalTarget.GetQuery());
        }
        if (originalTarget.HasFragment()) {
            request.target.SetFragment(originalTarget.GetFragment());
        }
        const auto requestEncoding = request.Generate();
        connectionState->connection->SendData({requestEncoding.begin(), requestEncoding.end()});
        impl_->AddTransaction(transaction);
        return transaction;
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
