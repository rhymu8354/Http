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

    std::string Response::Generate() const {
        std::ostringstream builder;
        builder << "HTTP/1.1 " << statusCode << ' ' << reasonPhrase << "\r\n";
        builder << headers.GenerateRawHeaders();
        builder << body;
        return builder.str();
    }

}
