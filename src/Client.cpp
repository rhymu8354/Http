/**
 * @file Client.cpp
 *
 * This module contains the implementation of the Http::Client class.
 *
 * © 2018 by Richard Walters
 */

#include <Http/Client.hpp>
#include <limits>
#include <string>
#include <sstream>
#include <SystemAbstractions/StringExtensions.hpp>

namespace {

    /**
     * This is the character sequence corresponding to a carriage return (CR)
     * followed by a line feed (LF), which officially delimits each
     * line of an HTTP response.
     */
    const std::string CRLF("\r\n");

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

}

namespace Http {

    /**
     * This contains the private properties of a Client instance.
     */
    struct Client::Impl {
    };

    Client::~Client() = default;

    Client::Client()
        : impl_(new Impl)
    {
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

        // First, extract and parse the status line.
        const auto statusLineEnd = rawResponse.find(CRLF);
        if (statusLineEnd == std::string::npos) {
            return nullptr;
        }
        const auto statusLine = rawResponse.substr(0, statusLineEnd);
        if (!ParseStatusLine(*response, statusLine)) {
            return nullptr;
        }

        // Second, parse the message headers and identify where the body begins.
        const auto headersOffset = statusLineEnd + CRLF.length();
        size_t bodyOffset;
        switch (
            response->headers.ParseRawMessage(
                rawResponse.substr(headersOffset),
                bodyOffset
            )
        ) {
            case MessageHeaders::MessageHeaders::State::Complete: {
                if (!response->headers.IsValid()) {
                    return nullptr;
                }
            } break;
            case MessageHeaders::MessageHeaders::State::Incomplete: return nullptr;
            case MessageHeaders::MessageHeaders::State::Error: return nullptr;
            default: return nullptr;
        }

        // Check for "Content-Length" header.  If present, use this to
        // determine how many characters should be in the body.
        bodyOffset += headersOffset;
        const auto maxContentLength = rawResponse.length() - bodyOffset;

        // Finally, extract the body.  If there is a "Content-Length"
        // header, we carefully carve exactly that number of characters
        // out (and bail if we don't have enough).  Otherwise, we just
        // assume the body extends to the end of the raw message.
        if (response->headers.HasHeader("Content-Length")) {
            intmax_t contentLengthAsInt;
            if (
                SystemAbstractions::ToInteger(
                    response->headers.GetHeaderValue("Content-Length"),
                    contentLengthAsInt
                ) != SystemAbstractions::ToIntegerResult::Success
            ) {
                return nullptr;
            }
            if (contentLengthAsInt < 0) {
                return nullptr;
            } else if (contentLengthAsInt > (intmax_t)maxContentLength) {
                return nullptr;
            } else {
                const auto contentLength = (size_t)contentLengthAsInt;
                response->body = rawResponse.substr(bodyOffset, contentLength);
                messageEnd = bodyOffset + contentLength;
            }
        } else {
            response->body = rawResponse.substr(bodyOffset);
            messageEnd = bodyOffset;
        }
        return response;
    }
}
