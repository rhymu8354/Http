/**
 * @file Server.cpp
 *
 * This module contains the implementation of the Http::Server class.
 *
 * © 2018 by Richard Walters
 */

#include "Deflate.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <Http/Server.hpp>
#include <inttypes.h>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <sstream>
#include <stddef.h>
#include <stdio.h>
#include <string>
#include <StringExtensions/StringExtensions.hpp>
#include <thread>

namespace {

    /**
     * This is the character sequence corresponding to a carriage return (CR)
     * followed by a line feed (LF), which officially delimits each
     * line of an HTTP request.
     */
    const std::string CRLF("\r\n");

    /**
     * This is the default maximum allowed total message size.
     *
     * Make sure this is never larger than the largest value
     * for the size_t type.
     */
    constexpr intmax_t DEFAULT_MAX_MESSAGE_SIZE = 10000000;

    /**
     * This is the default maximum length allowed for a request header line.
     */
    constexpr size_t DEFAULT_HEADER_LINE_LIMIT = 1000;

    /**
     * This is the default number of bytes of every bad request to
     * be reported in diagnostic messages.
     */
    constexpr size_t DEFAULT_BAD_REQUEST_REPORT_BYTES = 100;

    /**
     * This is the default public port number to which clients may connect
     * to establish connections with this server.
     */
    constexpr uint16_t DEFAULT_PORT_NUMBER = 80;

    /**
     * This is the default maximum number of seconds to allow to elapse
     * between receiving one byte of a client request and
     * receiving the next byte, before timing out.
     */
    constexpr double DEFAULT_INACTIVITY_TIMEOUT_SECONDS = 1.0;

    /**
     * This is the default maximum number of seconds to allow to elapse
     * between initiating a graceful close on connection and
     * forcing the connection closed ungracefully.
     */
    constexpr double DEFAULT_GRACEFUL_CLOSE_TIMEOUT_SECONDS = 1.0;

    /**
     * This is the default maximum number of seconds to allow to elapse
     * between receiving the first byte of a client request and
     * receiving the last byte of the request, before timing out.
     */
    constexpr double DEFAULT_REQUEST_TIMEOUT_SECONDS = 60.0;

    /**
     * This is the default maximum number of seconds to allow to elapse
     * between the end of a request, or the beginning of a connection,
     * and receiving the first byte of a new client request,
     * before timing out.
     */
    constexpr double DEFAULT_IDLE_TIMEOUT_SECONDS = 60.0;

    /**
     * This is the default number of seconds for which a client should
     * be banned in response to sending a bad request or too many requests.
     */
    constexpr double DEFAULT_BAN_PERIOD_SECONDS = 60.0;

    /**
     * This is the default number of seconds after a client's ban is
     * lifted before the client's ban period will be reset.
     */
    constexpr double DEFAULT_PROBATION_PERIOD_SECONDS = 60.0;

    /**
     * This is the default threshold, in requests per second, above which
     * a client will be banned.
     */
    constexpr double DEFAULT_TOO_MANY_REQUESTS_THRESHOLD = 10.0;

    /**
     * This is the period over which the number of client requests is
     * measured.
     */
    constexpr double DEFAULT_TOO_MANY_REQUESTS_MEASUREMENT_PERIOD = 1.0;

    /**
     * This is the default threshold, in connect requests per second, above
     * which client requests will be denied.
     */
    constexpr double DEFAULT_TOO_MANY_CONNECTS_THRESHOLD = 100.0;

    /**
     * This is the period over which the number of connect requests is
     * measured.
     */
    constexpr double DEFAULT_TOO_MANY_CONNECTS_MEASUREMENT_PERIOD = 1.0;

    /**
     * This is used to indicate how to handle the server's end
     * of a connection.
     */
    enum class ServerConnectionEndHandling {
        CloseGracefully,
        CloseAbruptly
    };

    /**
     * This is a temporary wrapper to adapt the Http::TimeKeeper interface
     * to the Timekeeping::Clock interface.  It will be removed once the
     * Http::TimeKeeper interface becomes obsolete.
     */
    struct ClockWrapper
        : public Timekeeping::Clock
    {
        // Properties

        /**
         * This is the timekeeper wrapped by this adapter.
         */
        std::shared_ptr< Http::TimeKeeper > timeKeeper;

        // Methods

        /**
         * Construct a new wrapper for the given timekeeper.
         *
         * @param[in] timeKeeper
         *     This is the timekeeper wrapped by this adapter.
         */
        explicit ClockWrapper(std::shared_ptr< Http::TimeKeeper > timeKeeper)
            : timeKeeper(timeKeeper)
        {
        }

        // Timekeeping::Clock

        virtual double GetCurrentTime() override {
            return timeKeeper->GetCurrentTime();
        }
    };

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
     * This is a helper function which formats the given double-precision
     * floating-point value as a string, ensuring that it will always
     * look like a floating-point value, and never an integer.
     *
     * @param[in] number
     *     This is the number to format.
     *
     * @return
     *     A string representation of the number, guaranteed not to
     *     look like an integer, is returned.
     */
    std::string FormatDoubleAsDistinctlyNotInteger(double number) {
        auto s = StringExtensions::sprintf("%.15lg", number);
        if (s.find_first_not_of("0123456789-") == std::string::npos) {
            s += ".0";
        }
        return s;
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
        Http::Request& request,
        const std::string& requestLine
    ) {
        // Parse the method.
        const auto methodDelimiter = requestLine.find(' ');
        if (methodDelimiter == std::string::npos) {
            return false;
        }
        request.method = requestLine.substr(0, methodDelimiter);
        if (request.method.empty()) {
            return false;
        }

        // Parse the target URI.
        const auto targetDelimiter = requestLine.find(' ', methodDelimiter + 1);
        if (targetDelimiter == std::string::npos) {
            return false;
        }
        const auto targetLength = targetDelimiter - methodDelimiter - 1;
        if (targetLength == 0) {
            return false;
        }
        if (
            !request.target.ParseFromString(
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
         * When a request is in progress, this is the token obtained
         * from the scheduler for the scheduled inactivity timeout callback.
         */
        int inactivityTimeoutToken = 0;

        /**
         * This is the time reported by the time keeper when
         * the current request was started.
         */
        double timeLastRequestStarted = 0.0;

        /**
         * When a request is in progress, this is the token obtained
         * from the scheduler for the scheduled request timeout callback.
         */
        int requestTimeoutToken = 0;

        /**
         * This is the time reported by the time keeper when
         * a graceful close was initiated on the connection.
         */
        double timeClosedGracefully = 0.0;

        /**
         * This flag indicates whether or not the client is currently
         * issuing a request to the server.
         */
        bool requestInProgress = false;

        /**
         * When no request is in progress, this is the token obtained
         * from the scheduler for the scheduled idle timeout callback.
         */
        int idleTimeoutToken = 0;

        /**
         * This buffer is used to reassemble fragmented HTTP requests
         * received from the client.
         */
        std::string reassemblyBuffer;

        /**
         * This holds the beginning of the current request, used to
         * report the bytes received for a bad request.
         */
        std::vector< uint8_t > requestExtract;

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

        /**
         * This flag indicates whether or not the server has closed
         * its end of the connection.
         */
        bool serverClosed = false;

        /**
         * This flag indicates whether or not the client has closed
         * its end of the connection.
         */
        bool clientClosed = false;
    };

    /**
     * This holds information about a client of the web server.
     */
    struct ClientDossier {
        /**
         * This is the number of seconds that the client will be
         * ignored, counting from the moment the ban was imposed.
         */
        double banPeriod = DEFAULT_BAN_PERIOD_SECONDS;

        /**
         * This is a sampling of the time, according to the server's
         * timekeeper, when this client was last banned.
         */
        double banStart = 0.0;

        /**
         * This flag indicates whether or not the client
         * is currently banned or on probation.
         */
        bool banned = false;

        /**
         * These are samples of the time keeper at each point
         * when the client made a request in the past.
         */
        std::deque< double > lastRequestTimes;
    };

    /**
     * This holds the arguments to pass when calling any registered
     * ban delegates.
     */
    struct BanDelegateArguments {
        /**
         * This is the address of the peer whose connections should be banned.
         */
        std::string peerAddress;

        /**
         * This is an explanation of the ban to report through the diagnostics
         * system.
         */
        std::string reason;
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
         * This is the maximum allowed total message size.
         */
        size_t maxMessageSize = DEFAULT_MAX_MESSAGE_SIZE;

        /**
         * This is the maximum number of characters allowed on any header line
         * of an HTTP request.
         */
        size_t headerLineLimit = DEFAULT_HEADER_LINE_LIMIT;

        /**
         * This is the number of bytes of every bad request to
         * be reported in diagnostic messages.
         */
        size_t badRequestReportBytes = DEFAULT_BAD_REQUEST_REPORT_BYTES;

        /**
         * This is the public port number to which clients may connect
         * to establish connections with this server.
         */
        uint16_t port = DEFAULT_PORT_NUMBER;

        /**
         * This is the default maximum number of seconds to allow to elapse
         * between the end of a request, or the beginning of a connection,
         * and receiving the first byte of a new client request,
         * before timing out.
         */
        double idleTimeout = DEFAULT_IDLE_TIMEOUT_SECONDS;

        /**
         * This is the maximum number of seconds to allow to elapse
         * between receiving one byte of a client request and
         * receiving the next byte, before timing out.
         */
        double inactivityTimeout = DEFAULT_INACTIVITY_TIMEOUT_SECONDS;

        /**
         * This is the maximum number of seconds to allow to elapse
         * between initiating a graceful close on connection and
         * forcing the connection closed ungracefully.
         */
        double gracefulCloseTimeout = DEFAULT_GRACEFUL_CLOSE_TIMEOUT_SECONDS;

        /**
         * This is the maximum number of seconds to allow to elapse
         * between receiving the first byte of a client request and
         * receiving the last byte of the request, before timing out.
         */
        double requestTimeout = DEFAULT_REQUEST_TIMEOUT_SECONDS;

        /**
         * This is the amount of time a client should be initially banned
         * in response to a bad request or too many requests.
         */
        double initialBanPeriod = DEFAULT_BAN_PERIOD_SECONDS;

        /**
         * This is the amount of time required after a client's ban
         * has been lifted, before the client's ban period will be reset.
         */
        double probationPeriod = DEFAULT_PROBATION_PERIOD_SECONDS;

        /**
         * This is the threshold, in requests per second, after which
         * a client will be banned.
         */
        double tooManyRequestsThreshold = DEFAULT_TOO_MANY_REQUESTS_THRESHOLD;

        /**
         * This is the period of time over which the number of requests
         * made by a client is measured.
         */
        double tooManyRequestsMeasurementPeriod = DEFAULT_TOO_MANY_REQUESTS_MEASUREMENT_PERIOD;

        /**
         * This is the threshold, in connect requests per second, after which
         * new requests will be denied.
         */
        double tooManyConnectsThreshold = DEFAULT_TOO_MANY_CONNECTS_THRESHOLD;

        /**
         * This is the period of time over which the number of connect requests
         * by clients is measured.
         */
        double tooManyConnectsMeasurementPeriod = DEFAULT_TOO_MANY_CONNECTS_MEASUREMENT_PERIOD;

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
         * This is used to schedule work to be done without having to
         * poll the timekeeper.
         */
        std::unique_ptr< Timekeeping::Scheduler > scheduler;

        /**
         * This holds information about known clients of the server.
         */
        std::map< std::string, ClientDossier > clients;

        /**
         * This holds the addresses of clients that have been "acceptlisted",
         * or are immune to bans and not checked against rate limits.
         */
        std::set< std::string > acceptlist;

        /**
         * These are the currently active client connections.
         */
        std::set< std::shared_ptr< ConnectionState > > activeConnections;

        /**
         * These are the client connections that are no longer
         * managed by the server (because they have either broken or
         * have been upgraded and are now handled by a resource) and will
         * be dropped by the reaper thread.
         */
        std::set< std::shared_ptr< ConnectionState > > connectionsToDrop;

        /**
         * This holds the arguments for any ban delegate calls that have
         * been queued to be called by the reaper thread.
         */
        std::queue< BanDelegateArguments > queuedBanDelegateCallArguments;

        /**
         * This holds all registered ban delegates.
         */
        std::map< int, BanDelegate > banDelegates;

        /**
         * This is the next unique identification number to assign to
         * a registered ban delegate.
         */
        int nextBanDelegateId = 1;

        /**
         * This is a helper object used to generate and publish
         * diagnostic messages.
         */
        SystemAbstractions::DiagnosticsSender diagnosticsSender;

        /**
         * These are samples of the time keeper at each point
         * when a connection request was made a request in the past.
         */
        std::deque< double > lastConnectTimes;

        /**
         * This flag indicates whether or not the last connection request
         * was denied due to exceeding the connection request rate limit.
         */
        bool connectRateLimited = false;

        /**
         * This is a worker thread which clears the connectionsToDrop set
         * and completes any queued delegate calls.  This prevents deadlocks
         * since these activities must be done without holding the server's
         * mutex.
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
        std::recursive_mutex mutex;

        /**
         * This is used by the reaper thread to wait on any
         * condition that it should cause it to wake up.
         */
        std::condition_variable_any reaperWakeCondition;

        // Methods

        /**
         * This is the constructor for the structure.
         */
        Impl()
            : diagnosticsSender("Http::Server")
        {
        }

        /**
         * This is the template of a helper function which is used to
         * parse a configuration item and set it if the parsing is successful.
         *
         * @param ItemType
         *     This is the type of the configuration item.
         *
         * @param[in,out] item
         *     This is the configuration item to set.
         *
         * @param[in] scanFormat
         *     This is the scanf-style format specification for printing
         *     the configuration item.
         *
         * @param[in] printFormat
         *     This is the printf-style format specification for printing
         *     the configuration item.
         *
         * @param[in] description
         *     This is the string to display in diagnostic messages about
         *     the configuration item.
         *
         * @param[in] value
         *     This is the value to parse to be the new value of the item.
         */
        template<
            typename ItemType
        > void ParseConfigurationItem(
            ItemType& item,
            const char* const scanFormat,
            const char* const printFormat,
            const char* const description,
            const std::string& value
        ) {
            ItemType newItem;
            if (
                sscanf(
                    value.c_str(),
                    scanFormat,
                    &newItem
                ) == 1
            ) {
                if (item != newItem) {
                    diagnosticsSender.SendDiagnosticInformationFormatted(
                        0,
                        StringExtensions::sprintf(
                            "%s changed from %s to %s",
                            description,
                            printFormat,
                            printFormat
                        ).c_str(),
                        item,
                        newItem
                    );
                    item = newItem;
                }
            }
        }

        /**
         * This method is the body of the reaper thread.
         * Until it's told to stop, it simply clears
         * the connectionsToDrop set whenever it wakes up.
         */
        void Reaper() {
            std::unique_lock< decltype(mutex) > lock(mutex);
            while (!stopReaper) {
                std::set< std::shared_ptr< ConnectionState > > oldConnectionsToDrop(std::move(connectionsToDrop));
                connectionsToDrop.clear();
                {
                    lock.unlock();
                    oldConnectionsToDrop.clear();
                    lock.lock();
                }
                std::queue< BanDelegateArguments > banDelegateCallArguments;
                banDelegateCallArguments.swap(queuedBanDelegateCallArguments);
                const auto banDelegatesCopy = banDelegates;
                {
                    lock.unlock();
                    while (!banDelegateCallArguments.empty()) {
                        const auto& arguments = banDelegateCallArguments.front();
                        for (const auto& banDelegate: banDelegatesCopy) {
                            banDelegate.second(
                                arguments.peerAddress,
                                arguments.reason
                            );
                        }
                        banDelegateCallArguments.pop();
                    }
                    lock.lock();
                }
                reaperWakeCondition.wait(
                    lock,
                    [this]{
                        return (
                            stopReaper
                            || !connectionsToDrop.empty()
                            || !queuedBanDelegateCallArguments.empty()
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
            Request& request,
            const std::string& nextRawRequestPart
        ) {
            // Count the number of characters incorporated into
            // the request object.
            size_t messageEnd = 0;

            // First, extract and parse the request line.
            if (request.state == Request::State::RequestLine) {
                const auto requestLineEnd = nextRawRequestPart.find(CRLF);
                if (requestLineEnd == std::string::npos) {
                    if (nextRawRequestPart.length() > headerLineLimit) {
                        request.state = Request::State::Error;
                        return messageEnd;
                    }
                    return messageEnd;
                }
                const auto requestLineLength = requestLineEnd;
                if (requestLineLength > headerLineLimit) {
                    request.state = Request::State::Error;
                    return messageEnd;
                }
                const auto requestLine = nextRawRequestPart.substr(0, requestLineLength);
                messageEnd = requestLineEnd + CRLF.length();
                request.totalBytes = messageEnd;
                request.state = Request::State::Headers;
                request.valid = ParseRequestLine(request, requestLine);
            }

            // Second, parse the message headers and identify where the body begins.
            if (request.state == Request::State::Headers) {
                request.headers.SetLineLimit(headerLineLimit);
                size_t headerBytesConsumed;
                const auto headersState = request.headers.ParseRawMessage(
                    nextRawRequestPart.substr(messageEnd),
                    headerBytesConsumed
                );
                request.totalBytes += headerBytesConsumed;
                messageEnd += headerBytesConsumed;
                if (request.totalBytes > maxMessageSize) {
                    request.state = Request::State::Error;
                    request.responseStatusCode = 431;
                    request.responseReasonPhrase = "Request Header Fields Too Large";
                    return messageEnd;
                }
                switch (headersState) {
                    case MessageHeaders::MessageHeaders::State::Complete: {
                        // Done with parsing headers; next will be body.
                        if (!request.headers.IsValid()) {
                            request.valid = false;
                        }
                        request.state = Request::State::Body;

                        // Check for "Host" header.
                        if (request.headers.HasHeader("Host")) {
                            const auto requestHost = request.headers.GetHeaderValue("Host");
                            auto serverHost = configuration["host"];
                            if (serverHost.empty()) {
                                serverHost = requestHost;
                            }
                            auto targetHost = request.target.GetHost();
                            if (targetHost.empty()) {
                                targetHost = serverHost;
                            }
                            if (
                                (requestHost != targetHost)
                                || (requestHost != serverHost)
                            ) {
                                request.valid = false;
                            }
                        } else {
                            request.valid = false;
                        }
                    } break;

                    case MessageHeaders::MessageHeaders::State::Incomplete: {
                    } return messageEnd;

                    case MessageHeaders::MessageHeaders::State::Error:
                    default: {
                        request.state = Request::State::Error;
                        return messageEnd;
                    }
                }
            }

            // Finally, extract the body.
            if (request.state == Request::State::Body) {
                // If there is a "Content-Length"
                // header, we carefully carve exactly that number of characters
                // out (and bail if we don't have enough).  Otherwise, we just
                // assume the body extends to the end of the raw message.
                const auto bytesAvailableForBody = nextRawRequestPart.length() - messageEnd;
                if (request.headers.HasHeader("Content-Length")) {
                    intmax_t contentLengthAsInt;
                    switch (
                        StringExtensions::ToInteger(
                            request.headers.GetHeaderValue("Content-Length"),
                            contentLengthAsInt
                        )
                    ) {
                        case StringExtensions::ToIntegerResult::NotANumber: {
                            request.state = Request::State::Error;
                        } return messageEnd;

                        case StringExtensions::ToIntegerResult::Overflow: {
                            request.state = Request::State::Error;
                            request.responseStatusCode = 413;
                            request.responseReasonPhrase = "Payload Too Large";
                        } return messageEnd;

                        default: break;
                    }
                    if (contentLengthAsInt < 0) {
                        request.state = Request::State::Error;
                        return messageEnd;
                    }
                    request.totalBytes += (size_t)contentLengthAsInt;
                    if (request.totalBytes > maxMessageSize) {
                        request.state = Request::State::Error;
                        request.responseStatusCode = 413;
                        request.responseReasonPhrase = "Payload Too Large";
                        return messageEnd;
                    }
                    const auto contentLength = (size_t)contentLengthAsInt;
                    if (contentLength > bytesAvailableForBody) {
                        request.state = Request::State::Body;
                        return messageEnd;
                    } else {
                        request.body = nextRawRequestPart.substr(messageEnd, contentLength);
                        messageEnd += contentLength;
                        request.state = Request::State::Complete;
                    }
                } else {
                    request.body.clear();
                    request.state = Request::State::Complete;
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
                *connectionState->nextRequest,
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
            connectionState->requestInProgress = !connectionState->reassemblyBuffer.empty();
            if (connectionState->idleTimeoutToken != 0) {
                scheduler->Cancel(connectionState->idleTimeoutToken);
                connectionState->idleTimeoutToken = 0;
            }
            if (connectionState->inactivityTimeoutToken != 0) {
                scheduler->Cancel(connectionState->inactivityTimeoutToken);
                connectionState->inactivityTimeoutToken = 0;
            }
            if (connectionState->requestTimeoutToken != 0) {
                scheduler->Cancel(connectionState->requestTimeoutToken);
                connectionState->requestTimeoutToken = 0;
            }
            std::weak_ptr< ConnectionState > connectionStateWeak(connectionState);
            const auto timeoutCallback = [
                this,
                connectionStateWeak
            ]{
                auto connectionState = connectionStateWeak.lock();
                if (connectionState != nullptr) {
                    IssueTimeoutResponse(connectionState);
                }
            };
            if (connectionState->requestInProgress) {
                connectionState->requestTimeoutToken = scheduler->Schedule(
                    timeoutCallback,
                    now + requestTimeout
                );
                connectionState->inactivityTimeoutToken = scheduler->Schedule(
                    timeoutCallback,
                    now + inactivityTimeout
                );
            } else {
                connectionState->idleTimeoutToken = scheduler->Schedule(
                    timeoutCallback,
                    now + idleTimeout
                );
            }
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
         *
         * @param[in] emitDiagnosticMessage
         *     This flag indicates whether or not to publish a diagnostic
         *     message about this response being issued.
         */
        void IssueResponse(
            std::shared_ptr< ConnectionState > connectionState,
            Response& response,
            bool emitDiagnosticMessage
        ) {
            if (
                !response.headers.HasHeader("Transfer-Encoding")
                && !response.body.empty()
                && !response.headers.HasHeader("Content-Length")
            ) {
                response.headers.AddHeader(
                    "Content-Length",
                    StringExtensions::sprintf("%zu", response.body.length())
                );
            }
            const auto responseText = response.Generate();
            connectionState->connection->SendData(
                std::vector< uint8_t >(
                    responseText.begin(),
                    responseText.end()
                )
            );
            if (emitDiagnosticMessage) {
                diagnosticsSender.SendDiagnosticInformationFormatted(
                    1, "Sent %u '%s' response back to %s",
                    response.statusCode,
                    response.reasonPhrase.c_str(),
                    connectionState->connection->GetPeerId().c_str()
                );
            }
            bool closeRequested = false;
            if (
                (response.statusCode == 400)
                || (response.statusCode == 413)
                || (response.statusCode == 431)
            ) {
                closeRequested = true;
                const auto clientAddress = connectionState->connection->GetPeerAddress();
                BanHammer(
                    clientAddress,
                    StringExtensions::sprintf(
                        "Bad HTTP: %u %s",
                        response.statusCode,
                        response.reasonPhrase.c_str()
                    )
                );
            } else {
                closeRequested = response.headers.HasHeaderToken("Connection", "close");
            }
            if (closeRequested) {
                connectionState->acceptingRequests = false;
                connectionState->serverClosed = true;
                OnConnectionBroken(
                    connectionState,
                    "closed by server",
                    ServerConnectionEndHandling::CloseGracefully
                );
            }
        }

        /**
         * This method sends a request timeout response back to the given
         * client.
         *
         * @param[in] connectionState
         *     This is the state of the connection for which to issue
         *     the given response.
         */
        void IssueTimeoutResponse(
            std::shared_ptr< ConnectionState > connectionState
        ) {
            const auto response = std::make_shared< Response >();
            response->statusCode = 408;
            response->reasonPhrase = "Request Timeout";
            response->headers.AddHeader("Connection", "close");
            IssueResponse(connectionState, *response, true);
        }

        /**
         * This method bans the given client from the server.
         *
         * @note
         *     Acceptlisted clients cannot be banned.
         *
         * @param[in] clientAddress
         *     This is the address of the client to ban.
         *
         * @param[in] reason
         *     This is an explanation of the ban to report through
         *     the diagnostics system.
         */
        void BanHammer(
            const std::string& clientAddress,
            const std::string& reason
        ) {
            if (acceptlist.find(clientAddress) != acceptlist.end()) {
                diagnosticsSender.SendDiagnosticInformationFormatted(
                    3, "Request: %s would have been banned (%s), but is acceptlisted",
                    clientAddress.c_str(),
                    reason.c_str()
                );
                return;
            }
            BanDelegateArguments banDelegateArguments;
            banDelegateArguments.peerAddress = clientAddress;
            banDelegateArguments.reason = reason;
            queuedBanDelegateCallArguments.push(banDelegateArguments);
            reaperWakeCondition.notify_all();
            const auto now = timeKeeper->GetCurrentTime();
            auto& client = clients[clientAddress];
            if (client.banned) {
                client.banPeriod *= 2.0;
                diagnosticsSender.SendDiagnosticInformationFormatted(
                    3, "Request: %s ban extended to %lg seconds (%s)",
                    clientAddress.c_str(),
                    client.banPeriod,
                    reason.c_str()
                );
            } else {
                client.banPeriod = initialBanPeriod;
                diagnosticsSender.SendDiagnosticInformationFormatted(
                    3, "Request: %s banned for %lg seconds (%s)",
                    clientAddress.c_str(),
                    client.banPeriod,
                    reason.c_str()
                );
            }
            client.banStart = now;
            client.banned = true;
        }

        /**
         * This method publishes a diagnostic message about a client request.
         *
         * @param[in] request
         *     This is the request from the client, with the target
         *     possibly modified.
         *
         * @param[in] response
         *     This is the response that was constructed for the request.
         *
         * @param[in] target
         *     This is the original target URI rendered as a string.
         *
         * @param[in] peerId
         *     This is the identifier of the peer who sent the request.
         */
        void ReportRequest(
            const Request& request,
            const Response& response,
            const std::string& target,
            const std::string& peerId
        ) {
            diagnosticsSender.SendDiagnosticInformationFormatted(
                1, "Request: %s '%s' (%s) from %s: %u (%s)",
                request.method.c_str(),
                target.c_str(),
                (
                    request.headers.HasHeader("Content-Type")
                    ? StringExtensions::sprintf(
                        "%s:%zu",
                        request.headers.GetHeaderValue("Content-Type").c_str(),
                        request.body.length()
                    ).c_str()
                    : StringExtensions::sprintf(
                        "%zu",
                        request.body.length()
                    ).c_str()
                ),
                peerId.c_str(),
                response.statusCode,
                (
                    response.headers.HasHeader("Content-Type")
                    ? StringExtensions::sprintf(
                        "%s:%zu",
                        response.headers.GetHeaderValue("Content-Type").c_str(),
                        response.body.length()
                    ).c_str()
                    : StringExtensions::sprintf(
                        "%zu",
                        response.body.length()
                    ).c_str()
                )
            );
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
         *
         * @param[in] serverConnectionEndHandling
         *     This indicates how the server should handle the closing
         *     of its end of the connection.
         */
        void OnConnectionBroken(
            std::shared_ptr< ConnectionState > connectionState,
            const std::string& reason,
            ServerConnectionEndHandling serverConnectionEndHandling
        ) {
            diagnosticsSender.SendDiagnosticInformationFormatted(
                2, "Connection to %s %s",
                connectionState->connection->GetPeerId().c_str(),
                reason.c_str()
            );
            switch (serverConnectionEndHandling) {
                case ServerConnectionEndHandling::CloseGracefully: {
                    connectionState->connection->Break(true);
                    connectionState->timeClosedGracefully = timeKeeper->GetCurrentTime();
                    std::weak_ptr< ConnectionState > connectionStateWeak(connectionState);
                    scheduler->Schedule(
                        [
                            this,
                            connectionStateWeak
                        ]{
                            auto connectionState = connectionStateWeak.lock();
                            if (connectionState == nullptr) {
                                return;
                            }
                            if (connectionState->clientClosed) {
                                return;
                            }
                            OnConnectionBroken(
                                connectionState,
                                "forceably closed by server after graceful close timeout",
                                ServerConnectionEndHandling::CloseAbruptly
                            );
                        },
                        timeKeeper->GetCurrentTime() + gracefulCloseTimeout
                    );
                } break;

                case ServerConnectionEndHandling::CloseAbruptly: {
                    connectionState->connection->Break(false);
                    (void)connectionsToDrop.insert(connectionState);
                    reaperWakeCondition.notify_all();
                    (void)activeConnections.erase(connectionState);
                } break;
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
            const std::vector< uint8_t >& data
        ) {
            if (!connectionState->acceptingRequests) {
                return;
            }
            const auto now = timeKeeper->GetCurrentTime();
            connectionState->requestInProgress = true;
            if (connectionState->idleTimeoutToken != 0) {
                scheduler->Cancel(connectionState->idleTimeoutToken);
                connectionState->idleTimeoutToken = 0;
            }
            connectionState->timeLastDataReceived = now;
            if (connectionState->inactivityTimeoutToken != 0) {
                scheduler->Cancel(connectionState->inactivityTimeoutToken);
                connectionState->inactivityTimeoutToken = 0;
            }
            connectionState->reassemblyBuffer += std::string(data.begin(), data.end());
            if (connectionState->requestExtract.size() < badRequestReportBytes) {
                connectionState->requestExtract.insert(
                    connectionState->requestExtract.end(),
                    data.begin(),
                    data.begin() + std::min(
                        data.size(),
                        badRequestReportBytes - connectionState->requestExtract.size()
                    )
                );
            }
            while (connectionState->acceptingRequests) {
                const auto request = TryRequestAssembly(connectionState);
                if (request == nullptr) {
                    break;
                }
                const auto clientAddress = connectionState->connection->GetPeerAddress();
                auto& client = clients[clientAddress];
                Response response;
                if (
                    (tooManyRequestsThreshold != 0.0)
                    && (acceptlist.find(clientAddress) == acceptlist.end())
                    && !CheckRequestFrequency(client)
                ) {
                    response.statusCode = 429;
                    response.reasonPhrase = "Too Many Requests";
                    response.headers.SetHeader("Connection", "close");
                    ReportRequest(
                        *request,
                        response,
                        request->target.GenerateString(),
                        connectionState->connection->GetPeerId()
                    );
                    BanHammer(clientAddress, "Bad HTTP: 429 Too Many Requests");
                } else if (
                    (request->state == Request::State::Complete)
                    && request->valid
                ) {
                    const auto originalTargetAsString = request->target.GenerateString();
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
                        response = resource->handler(*request, connectionState->connection, connectionState->reassemblyBuffer);
                        std::string codingsApplied;
                        for (const auto& coding: response.headers.GetHeaderTokens("Content-Encoding")) {
                            static const std::map< std::string, DeflateMode > deflateModesSupported{
                                {"gzip", DeflateMode::Gzip},
                                {"deflate", DeflateMode::Deflate},
                            };
                            const auto codingEntry = deflateModesSupported.find(coding);
                            if (codingEntry == deflateModesSupported.end()) {
                                if (codingsApplied.empty()) {
                                    codingsApplied = coding;
                                }
                            } else {
                                if (!codingsApplied.empty()) {
                                    codingsApplied += ", ";
                                }
                                codingsApplied += coding;
                                response.body = Deflate(response.body, codingEntry->second);
                                response.headers.SetHeader(
                                    "Content-Length",
                                    StringExtensions::sprintf("%zu", response.body.size())
                                );
                            }
                        }
                        if (codingsApplied.empty()) {
                            response.headers.RemoveHeader("Content-Encoding");
                        } else {
                            response.headers.SetHeader("Content-Encoding", codingsApplied);
                        }
                    } else {
                        response.statusCode = 404;
                        response.reasonPhrase = "Not Found";
                        response.headers.SetHeader("Content-Type", "text/plain");
                        response.body = "FeelsBadMan\r\n";
                    }
                    if (request->headers.HasHeaderToken("Connection", "close")) {
                        auto responseConnectionTokens = response.headers.GetHeaderTokens("Connection");
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
                        response.headers.SetHeader("Connection", responseConnectionTokens, true);
                    }
                    ReportRequest(
                        *request,
                        response,
                        originalTargetAsString,
                        connectionState->connection->GetPeerId()
                    );
                } else {
                    response.statusCode = request->responseStatusCode;
                    response.reasonPhrase = request->responseReasonPhrase;
                    response.headers.SetHeader("Content-Type", "text/plain");
                    response.body = "FeelsBadMan\r\n";
                    if (request->state == Request::State::Error) {
                        response.headers.SetHeader("Connection", "close");
                    }
                    std::ostringstream requestExtractStringBuilder;
                    requestExtractStringBuilder << std::hex << std::setfill('0');
                    for (auto ch: connectionState->requestExtract) {
                        if ((ch <= 0x20) || (ch > 0x7E)) {
                            requestExtractStringBuilder << "\\x" << std::setw(2) << (int)ch;
                        } else {
                            requestExtractStringBuilder << (char)ch;
                        }
                    }
                    diagnosticsSender.SendDiagnosticInformationFormatted(
                        3, "Request: Bad request from %s: %s",
                        connectionState->connection->GetPeerId().c_str(),
                        requestExtractStringBuilder.str().c_str()
                    );
                }
                if (connectionState->reassemblyBuffer.empty()) {
                    connectionState->requestExtract.clear();
                } else {
                    connectionState->requestExtract.assign(
                        connectionState->reassemblyBuffer.begin(),
                        connectionState->reassemblyBuffer.begin() + std::min(
                            connectionState->reassemblyBuffer.size(),
                            badRequestReportBytes
                        )
                    );
                }
                IssueResponse(connectionState, response, false);
                if (response.statusCode == 101) {
                    connectionState->acceptingRequests = false;
                    connectionState->connection = nullptr;
                    (void)connectionsToDrop.insert(connectionState);
                    reaperWakeCondition.notify_all();
                    (void)activeConnections.erase(connectionState);
                }
            }
            if (
                connectionState->requestInProgress
                && (connectionState->inactivityTimeoutToken == 0)
            ) {
                std::weak_ptr< ConnectionState > connectionStateWeak(connectionState);
                const auto timeoutCallback = [
                    this,
                    connectionStateWeak
                ]{
                    auto connectionState = connectionStateWeak.lock();
                    if (connectionState != nullptr) {
                        IssueTimeoutResponse(connectionState);
                    }
                };
                connectionState->requestTimeoutToken = scheduler->Schedule(
                    timeoutCallback,
                    now + requestTimeout
                );
                connectionState->inactivityTimeoutToken = scheduler->Schedule(
                    timeoutCallback,
                    now + inactivityTimeout
                );
            }
        }

        /**
         * Check to see if the connection from the given client should be
         * denied due to exceeding the connection rate limit.
         *
         * @param[in] client
         *     This is the client whose connection is being evaluated.
         *
         * @return
         *     An indication of whether or not the client's connection
         *     should be accepted is returned.
         */
        bool CheckConnectFrequency(const ClientDossier& client) {
            const auto now = timeKeeper->GetCurrentTime();
            while (
                !lastConnectTimes.empty()
                && (lastConnectTimes.front() < now - tooManyConnectsMeasurementPeriod)
            ) {
                lastConnectTimes.pop_front();
            }
            const auto numConnectsAcrossMeasurementPeriod = lastConnectTimes.size();
            const auto averageConnectsPerSecond = (
                (double)numConnectsAcrossMeasurementPeriod
                / tooManyConnectsMeasurementPeriod
            );
            if (averageConnectsPerSecond >= tooManyConnectsThreshold) {
                if (!connectRateLimited) {
                    connectRateLimited = true;
                    diagnosticsSender.SendDiagnosticInformationString(
                        SystemAbstractions::DiagnosticsSender::Levels::WARNING,
                        "Incoming connection rate limit exceeded"
                    );
                }
                return false;
            }
            connectRateLimited = false;
            lastConnectTimes.push_back(now);
            return true;
        }

        /**
         * Check to see if the request from the given client should be
         * denied due to exceeding the request rate limit.
         *
         * @param[in,out] client
         *     This is the client whose request is being evaluated.
         *     Its history of request times may be updated.
         *
         * @return
         *     An indication of whether or not the client's
         *     request should be accepted is returned.
         */
        bool CheckRequestFrequency(ClientDossier& client) {
            const auto now = timeKeeper->GetCurrentTime();
            client.lastRequestTimes.push_back(now);
            while (client.lastRequestTimes.front() < now - tooManyRequestsMeasurementPeriod) {
                client.lastRequestTimes.pop_front();
            }
            const auto numClientRequestsAcrossMeasurementPeriod = client.lastRequestTimes.size();
            const auto averageClientRequestsPerSecond = (
                (double)numClientRequestsAcrossMeasurementPeriod
                / tooManyRequestsMeasurementPeriod
            );
            return (averageClientRequestsPerSecond < tooManyRequestsThreshold);
        }

        /**
         * This method is called when a new connection has been
         * established for the server.
         *
         * @param[in] connection
         *     This is the new connection has been established for the server.
         *
         * @return
         *     A delegate to be called when the connection is ready to be used
         *     is returned.
         */
        ServerTransport::ConnectionReadyDelegate NewConnection(std::shared_ptr< Connection > connection) {
            std::lock_guard< decltype(mutex) > lock(mutex);
            const auto clientAddress = connection->GetPeerAddress();
            auto& client = clients[clientAddress];
            if (client.banned) {
                const auto now = timeKeeper->GetCurrentTime();
                if (now < client.banStart + client.banPeriod) {
                    diagnosticsSender.SendDiagnosticInformationFormatted(
                        2, "New connection from %s -- banned",
                        connection->GetPeerId().c_str()
                    );
                    connection->Break(false);
                    return nullptr;
                } else if (now >= client.banStart + client.banPeriod + probationPeriod) {
                    client.banned = false;
                }
            }
            if (
                (acceptlist.find(clientAddress) == acceptlist.end())
                && !CheckConnectFrequency(client)
            ) {
                connection->Break(false);
                return nullptr;
            }
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
                [this, connectionStateWeak](const std::vector< uint8_t >& data){
                    const auto connectionState = connectionStateWeak.lock();
                    if (connectionState == nullptr) {
                        return;
                    }
                    std::lock_guard< decltype(mutex) > lock(mutex);
                    DataReceived(connectionState, data);
                }
            );
            connection->SetBrokenDelegate(
                [this, connectionStateWeak](bool){
                    const auto connectionState = connectionStateWeak.lock();
                    if (connectionState == nullptr) {
                        return;
                    }
                    std::lock_guard< decltype(mutex) > lock(mutex);
                    if (connectionState->serverClosed) {
                        if (!connectionState->clientClosed) {
                            connectionState->clientClosed = true;
                            OnConnectionBroken(
                                connectionState,
                                "peer end closed",
                                ServerConnectionEndHandling::CloseAbruptly
                            );
                        }
                    } else {
                        connectionState->serverClosed = true;
                        OnConnectionBroken(
                            connectionState,
                            "broken by peer",
                            ServerConnectionEndHandling::CloseAbruptly
                        );
                    }
                }
            );
            return nullptr;
        }
    };

    Server::~Server() noexcept {
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
        impl_->configuration["MaxMessageSize"] = StringExtensions::sprintf("%zu", impl_->maxMessageSize);
        impl_->configuration["HeaderLineLimit"] = StringExtensions::sprintf("%zu", impl_->headerLineLimit);
        impl_->configuration["Port"] = StringExtensions::sprintf("%" PRIu16, impl_->port);
        impl_->configuration["RequestTimeout"] = FormatDoubleAsDistinctlyNotInteger(impl_->requestTimeout);
        impl_->configuration["IdleTimeout"] = FormatDoubleAsDistinctlyNotInteger(impl_->idleTimeout);
        impl_->configuration["BadRequestReportBytes"] = StringExtensions::sprintf("%zu", impl_->badRequestReportBytes);
        impl_->configuration["InitialBanPeriod"] = FormatDoubleAsDistinctlyNotInteger(impl_->initialBanPeriod);
        impl_->configuration["ProbationPeriod"] = FormatDoubleAsDistinctlyNotInteger(impl_->probationPeriod);
        impl_->configuration["TooManyRequestsThreshold"] = FormatDoubleAsDistinctlyNotInteger(impl_->tooManyRequestsThreshold);
        impl_->configuration["TooManyRequestsMeasurementPeriod"] = FormatDoubleAsDistinctlyNotInteger(impl_->tooManyRequestsMeasurementPeriod);
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
                    return impl_->NewConnection(connection);
                }
            )
        ) {
            impl_->port = impl_->transport->GetBoundPort();
            impl_->diagnosticsSender.SendDiagnosticInformationFormatted(
                3, "Now listening on port %" PRIu16,
                impl_->port
            );
        } else {
            impl_->transport = nullptr;
            return false;
        }
        impl_->configuration["Port"] = StringExtensions::sprintf("%" PRIu16, impl_->port);
        impl_->timeKeeper = deps.timeKeeper;
        impl_->scheduler.reset(new Timekeeping::Scheduler);
        impl_->scheduler->SetClock(
            std::make_shared< ClockWrapper >(impl_->timeKeeper)
        );
        impl_->mobilized = true;
        return true;
    }

    void Server::Demobilize() {
        impl_->scheduler = nullptr;
        if (impl_->transport != nullptr) {
            impl_->transport->ReleaseNetwork();
            impl_->transport = nullptr;
        }
        impl_->timeKeeper = nullptr;
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
        messageEnd = impl_->ParseRequest(*request, rawRequest);
        if (request->IsCompleteOrError()) {
            return request;
        } else {
            return nullptr;
        }
    }

    Timekeeping::Scheduler& Server::GetScheduler() {
        return *impl_->scheduler;
    }

    SystemAbstractions::DiagnosticsSender::UnsubscribeDelegate Server::SubscribeToDiagnostics(
        SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate delegate,
        size_t minLevel
    ) {
        return impl_->diagnosticsSender.SubscribeToDiagnostics(delegate, minLevel);
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
        if (key == "MaxMessageSize") {
            impl_->ParseConfigurationItem(impl_->maxMessageSize, "%zu", "%zu", "Maximum message size", value);
        } else if (key == "HeaderLineLimit") {
            impl_->ParseConfigurationItem(impl_->headerLineLimit, "%zu", "%zu", "Header line limit", value);
        } else if (key == "Port") {
            impl_->ParseConfigurationItem(impl_->port, "%" SCNu16, "%" PRIu16, "Port number", value);
        } else if (key == "InactivityTimeout") {
            impl_->ParseConfigurationItem(impl_->inactivityTimeout, "%lf", "%lf", "Inactivity timeout", value);
        } else if (key == "GracefulCloseTimeout") {
            impl_->ParseConfigurationItem(impl_->gracefulCloseTimeout, "%lf", "%lf", "Graceful close timeout", value);
        } else if (key == "RequestTimeout") {
            impl_->ParseConfigurationItem(impl_->requestTimeout, "%lf", "%lf", "Request timeout", value);
        } else if (key == "IdleTimeout") {
            impl_->ParseConfigurationItem(impl_->idleTimeout, "%lf", "%lf", "Idle timeout", value);
        } else if (key == "BadRequestReportBytes") {
            impl_->ParseConfigurationItem(impl_->badRequestReportBytes, "%zu", "%zu", "Bad request report bytes", value);
        } else if (key == "InitialBanPeriod") {
            impl_->ParseConfigurationItem(impl_->initialBanPeriod, "%lf", "%lf", "Initial ban period", value);
        } else if (key == "ProbationPeriod") {
            impl_->ParseConfigurationItem(impl_->probationPeriod, "%lf", "%lf", "Probation period", value);
        } else if (key == "TooManyRequestsThreshold") {
            impl_->ParseConfigurationItem(impl_->tooManyRequestsThreshold, "%lf", "%lf", "Too many requests threshold", value);
        } else if (key == "TooManyRequestsMeasurementPeriod") {
            impl_->ParseConfigurationItem(impl_->tooManyRequestsMeasurementPeriod, "%lf", "%lf", "Too many requests measurement period", value);
        } else if (key == "TooManyConnectsThreshold") {
            impl_->ParseConfigurationItem(impl_->tooManyConnectsThreshold, "%lf", "%lf", "Too many connects threshold", value);
        } else if (key == "TooManyConnectsMeasurementPeriod") {
            impl_->ParseConfigurationItem(impl_->tooManyConnectsMeasurementPeriod, "%lf", "%lf", "Too many connects measurement period", value);
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
        if (space->handler == nullptr) {
            space->handler = resourceDelegate;
            return [this, space]{
                auto currentSpace = space;
                currentSpace->handler = nullptr;
                for (;;) {
                    auto superspace = currentSpace->superspace.lock();
                    if (
                        (currentSpace->handler == nullptr)
                        && currentSpace->subspaces.empty()
                    ) {
                        if (superspace == nullptr) { // at root level
                            impl_->resources = nullptr;
                            break;
                        } else {
                            (void)superspace->subspaces.erase(currentSpace->name);
                        }
                    }
                    if (
                        (superspace != nullptr)
                        && superspace->subspaces.empty()
                    ) {
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

    auto Server::RegisterBanDelegate(
        BanDelegate banDelegate
    ) -> UnregistrationDelegate {
        std::lock_guard< decltype(impl_->mutex) > lock(impl_->mutex);
        const auto id = impl_->nextBanDelegateId++;
        impl_->banDelegates[id] = banDelegate;
        std::weak_ptr< Impl > weakImpl(impl_);
        return [id, weakImpl]{
            auto impl = weakImpl.lock();
            if (!impl) {
                return;
            }
            std::lock_guard< decltype(impl->mutex) > lock(impl->mutex);
            (void)impl->banDelegates.erase(id);
        };
    }

    std::shared_ptr< TimeKeeper > Server::GetTimeKeeper() {
        return impl_->timeKeeper;
    }

    void Server::Ban(
        const std::string& peerAddress,
        const std::string& reason
    ) {
        std::lock_guard< decltype(impl_->mutex) > lock(impl_->mutex);
        impl_->BanHammer(peerAddress, reason);
    }

    void Server::Unban(const std::string& peerAddress) {
        std::lock_guard< decltype(impl_->mutex) > lock(impl_->mutex);
        auto& client = impl_->clients[peerAddress];
        client.banned = false;
    }

    std::set< std::string > Server::GetBans() {
        std::lock_guard< decltype(impl_->mutex) > lock(impl_->mutex);
        std::set< std::string > bans;
        const auto now = impl_->timeKeeper->GetCurrentTime();
        for (const auto& clientsEntry: impl_->clients) {
            const auto& client = clientsEntry.second;
            if (
                client.banned
                && (now < client.banStart + client.banPeriod)
            ) {
                (void)bans.insert(clientsEntry.first);
            }
        }
        return bans;
    }

    void Server::AcceptlistAdd(const std::string& peerAddress) {
        std::lock_guard< decltype(impl_->mutex) > lock(impl_->mutex);
        (void)impl_->acceptlist.insert(peerAddress);
    }

    void Server::AcceptlistRemove(const std::string& peerAddress) {
        std::lock_guard< decltype(impl_->mutex) > lock(impl_->mutex);
        (void)impl_->acceptlist.erase(peerAddress);
    }

    std::set< std::string > Server::GetAcceptlist() {
        std::lock_guard< decltype(impl_->mutex) > lock(impl_->mutex);
        return impl_->acceptlist;
    }

}
