/**
 * @file Client.cpp
 *
 * This module contains the implementation of the Http::Client class.
 *
 * © 2018 by Richard Walters
 */

#include <Http/Client.hpp>

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

}
