/**
 * @file RequestTests.cpp
 *
 * This module contains the unit tests of the
 * Http::Request structure.
 *
 * © 2018 by Richard Walters
 */

#include <gtest/gtest.h>
#include <Http/Request.hpp>

TEST(RequestTests, IsCompleteOrError) {
    Http::Request request;
    request.state = Http::Request::State::Complete;
    EXPECT_TRUE(request.IsCompleteOrError());
    request.state = Http::Request::State::Error;
    EXPECT_TRUE(request.IsCompleteOrError());
    request.state = Http::Request::State::Headers;
    EXPECT_FALSE(request.IsCompleteOrError());
    request.state = Http::Request::State::RequestLine;
    EXPECT_FALSE(request.IsCompleteOrError());
    request.state = Http::Request::State::Body;
    EXPECT_FALSE(request.IsCompleteOrError());
}
