/**
 * @file Request.cpp
 *
 * This module contains the implementation of the Http::Request structure.
 *
 * © 2018 by Richard Walters
 */

#include <Http/Request.hpp>
#include <sstream>

namespace Http {

    bool Request::IsCompleteOrError() const {
        return (
            (state == State::Complete)
            || (state == State::Error)
        );
    }

    std::string Request::Generate() const {
        std::ostringstream builder;
        builder << method << ' ' << target.GenerateString() << " HTTP/1.1\r\n";
        builder << headers.GenerateRawHeaders();
        builder << body;
        return builder.str();
    }

    void PrintTo(
        const Request::State& state,
        std::ostream* os
    ) {
        switch (state) {
            case Request::State::RequestLine: {
                *os << "Constructing Request line";
            } break;
            case Request::State::Headers: {
                *os << "Constructing Headers";
            } break;
            case Request::State::Body: {
                *os << "Constructing Body";
            } break;
            case Request::State::Complete: {
                *os << "COMPLETE";
            } break;
            case Request::State::Error: {
                *os << "ERROR";
            } break;
            default: {
                *os << "???";
            };
        }
    }

}
