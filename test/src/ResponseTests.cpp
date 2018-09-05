/**
 * @file ResponseTests.cpp
 *
 * This module contains the unit tests of the
 * Http::Response structure.
 *
 * © 2018 by Richard Walters
 */

#include <gtest/gtest.h>
#include <Http/Response.hpp>
#include <SystemAbstractions/StringExtensions.hpp>

TEST(ResponseTests, IsCompleteOrError) {
    Http::Response response;
    response.state = Http::Response::State::Complete;
    EXPECT_TRUE(response.IsCompleteOrError());
    response.state = Http::Response::State::Error;
    EXPECT_TRUE(response.IsCompleteOrError());
    response.state = Http::Response::State::Headers;
    EXPECT_FALSE(response.IsCompleteOrError());
    response.state = Http::Response::State::StatusLine;
    EXPECT_FALSE(response.IsCompleteOrError());
    response.state = Http::Response::State::Body;
    EXPECT_FALSE(response.IsCompleteOrError());
    EXPECT_TRUE(response.IsCompleteOrError(false));
    response = Http::Response();
    response.headers.SetHeader("Content-Length", "42");
    EXPECT_FALSE(response.IsCompleteOrError());
    EXPECT_FALSE(response.IsCompleteOrError(false));
    response = Http::Response();
    response.headers.SetHeader("Transfer-Encoding", "chunked");
    EXPECT_FALSE(response.IsCompleteOrError());
    EXPECT_FALSE(response.IsCompleteOrError(false));
}

TEST(ResponseTests, GenerateGetResponse) {
    Http::Response response;
    response.statusCode = 200;
    response.reasonPhrase = "OK";
    response.headers.AddHeader("Date", "Mon, 27 Jul 2009 12:28:53 GMT");
    response.headers.AddHeader("Accept-Ranges", "bytes");
    response.headers.AddHeader("Content-Type", "text/plain");
    response.body = "Hello World! My payload includes a trailing CRLF.\r\n";
    response.headers.AddHeader("Content-Length", SystemAbstractions::sprintf("%zu", response.body.size()));
    ASSERT_EQ(
        SystemAbstractions::sprintf(
            "HTTP/1.1 200 OK\r\n"
            "Date: Mon, 27 Jul 2009 12:28:53 GMT\r\n"
            "Accept-Ranges: bytes\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: %zu\r\n"
            "\r\n"
            "Hello World! My payload includes a trailing CRLF.\r\n",
            response.body.size()
        ),
        response.Generate()
    );
}
