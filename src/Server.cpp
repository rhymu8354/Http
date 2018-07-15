/**
 * @file Server.cpp
 *
 * This module contains the implementation of the Http::Server class.
 *
 * © 2018 by Richard Walters
 */

#include <Http/Server.hpp>
#include <memory>

namespace {

    /**
     * This is the character sequence corresponding to a carriage return (CR)
     * followed by a line feed (LF), which officially delimits each
     * line of an HTTP request.
     */
    const std::string CRLF("\r\n");

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

}

namespace Http {

    /**
     * This contains the private properties of a Server instance.
     */
    struct Server::Impl {
    };

    Server::~Server() = default;

    Server::Server()
        : impl_(new Impl)
    {
    }

    auto Server::ParseRequest(const std::string& rawRequest) -> std::shared_ptr< Request > {
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
        size_t bodyOffset;
        if (
            !request->headers.ParseRawMessage(
                rawRequest.substr(requestLineEnd + CRLF.length()),
                bodyOffset
            )
        ) {
            return nullptr;
        }

        // Finally, extract the body.
        request->body = rawRequest.substr(requestLineEnd + CRLF.length() + bodyOffset);
        return request;
    }

}
