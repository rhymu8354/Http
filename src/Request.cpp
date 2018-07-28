/**
 * @file Request.cpp
 *
 * This module contains the implementation of the Http::Request structure.
 *
 * © 2018 by Richard Walters
 */

#include <Http/Request.hpp>

namespace Http {

    bool Request::IsCompleteOrError() const {
        return (
            (state == State::Complete)
            || (state == State::Error)
        );
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
