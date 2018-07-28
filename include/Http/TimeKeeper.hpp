#ifndef HTTP_TIME_KEEPER_HPP
#define HTTP_TIME_KEEPER_HPP

/**
 * @file TimeKeeper.hpp
 *
 * This module declares the Http::TimeKeeper interface.
 *
 * © 2018 by Richard Walters
 */

#include "Connection.hpp"

#include <functional>
#include <memory>
#include <stdint.h>

namespace Http {

    /**
     * This represents the time-keeping requirements of Http::Server.
     * To integrate Http::Server into a larger program, implement this
     * interface in terms of the actual server time.
     */
    class TimeKeeper {
    public:
        // Methods

        /**
         * This method returns the current server time, in seconds.
         *
         * @return
         *     The current server time is returned, in seconds.
         */
        virtual double GetCurrentTime() = 0;
    };

}

#endif /* HTTP_TIME_KEEPER_HPP */
