/**
 * @file Response.cpp
 *
 * This module contains the implementation of the Http::Response structure.
 *
 * © 2018 by Richard Walters
 */

#include <Http/Response.hpp>
#include <sstream>

namespace Http {

    bool Response::IsCompleteOrError(bool moreDataPossible) const {
        if (
            (state == State::Complete)
            || (state == State::Error)
        ) {
            return true;
        }
        if (
            !moreDataPossible
            && (state == State::Body)
            && (
                !headers.HasHeader("Content-Length")
                && !headers.HasHeaderToken("Transfer-Encoding", "chunked")
            )
        ) {
            return true;
        }
        return false;
    }

    std::string Response::Generate() const {
        std::ostringstream builder;
        builder << "HTTP/1.1 " << statusCode << ' ' << reasonPhrase << "\r\n";
        builder << headers.GenerateRawHeaders();
        builder << body;
        return builder.str();
    }

}
