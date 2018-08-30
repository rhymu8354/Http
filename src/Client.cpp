/**
 * @file Client.cpp
 *
 * This module contains the implementation of the Http::Client class.
 *
 * © 2018 by Richard Walters
 */

#include <condition_variable>
#include <Http/Client.hpp>
#include <Http/Connection.hpp>
#include <limits>
#include <mutex>
#include <string>
#include <sstream>
#include <SystemAbstractions/DiagnosticsSender.hpp>
#include <SystemAbstractions/StringExtensions.hpp>

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

    struct TransactionImpl
        : public Http::Client::Transaction
    {
        // Properties

        /**
         * This is the connection to the server.
         */
        std::shared_ptr< Http::Connection > connection;

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
        std::mutex mutex;

        /**
         * This is used to wait for various object state changes.
         */
        std::condition_variable stateChange;

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
            connection->Break(false);
            connection = nullptr;
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

        // Methods

        /**
         * This is the constructor for the structure.
         */
        Impl()
            : diagnosticsSender("Http::Client")
        {
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
        impl_->mobilized = true;
    }

    auto Client::Request(Http::Request request) -> std::shared_ptr< Transaction > {
        const auto transaction = std::make_shared< TransactionImpl >();
        const auto& hostNameOrAddress = request.target.GetHost();
        auto port = DEFAULT_HTTP_PORT_NUMBER;
        if (request.target.HasPort()) {
            port = request.target.GetPort();
        }
        std::weak_ptr< TransactionImpl > transactionWeak(transaction);
        transaction->connection = impl_->transport->Connect(
            hostNameOrAddress,
            port,
            [transactionWeak](const std::vector< uint8_t >& data){
                const auto transaction = transactionWeak.lock();
                if (transaction == nullptr) {
                    return;
                }
                transaction->DataReceived(data);
            },
            [](bool){
            }
        );
        if (transaction->connection == nullptr) {
            transaction->response.statusCode = 503;
            transaction->response.reasonPhrase = "Service Unavailable";
            transaction->Complete();
        }
        request.headers.SetHeader("Host", hostNameOrAddress);
        request.headers.SetHeader("Connection", "Close");
        const auto requestEncoding = request.Generate();
        transaction->connection->SendData({requestEncoding.begin(), requestEncoding.end()});
        transaction->connection->Break(true);
        return transaction;
    }

    void Client::Demobilize() {
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
}
