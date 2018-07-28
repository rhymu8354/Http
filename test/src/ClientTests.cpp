/**
 * @file ClientTests.cpp
 *
 * This module contains the unit tests of the
 * Http::Client class.
 *
 * © 2018 by Richard Walters
 */

#include <gtest/gtest.h>
#include <Http/Client.hpp>

TEST(ClientTests, ParseGetRequest) {
    Http::Client client;
    const auto response = client.ParseResponse(
        "HTTP/1.1 200 OK\r\n"
        "Date: Mon, 27 Jul 2009 12:28:53 GMT\r\n"
        "Server: Apache\r\n"
        "Last-Modified: Wed, 22 Jul 2009 19:15:56 GMT\r\n"
        "ETag: \"34aa387-d-1568eb00\"\r\n"
        "Accept-Ranges: bytes\r\n"
        "Content-Length: 51\r\n"
        "Vary: Accept-Encoding\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "Hello World! My payload includes a trailing CRLF.\r\n"
    );
    ASSERT_FALSE(response == nullptr);
    ASSERT_EQ(200, response->statusCode);
    ASSERT_EQ("OK", response->reasonPhrase);
    ASSERT_TRUE(response->headers.HasHeader("Date"));
    ASSERT_EQ("Mon, 27 Jul 2009 12:28:53 GMT", response->headers.GetHeaderValue("Date"));
    ASSERT_TRUE(response->headers.HasHeader("Accept-Ranges"));
    ASSERT_EQ("bytes", response->headers.GetHeaderValue("Accept-Ranges"));
    ASSERT_TRUE(response->headers.HasHeader("Content-Type"));
    ASSERT_EQ("text/plain", response->headers.GetHeaderValue("Content-Type"));
    ASSERT_EQ("Hello World! My payload includes a trailing CRLF.\r\n", response->body);
}

TEST(ClientTests, ParseIncompleteBodyRequest) {
    Http::Client client;
    const auto response = client.ParseResponse(
        "HTTP/1.1 200 OK\r\n"
        "Date: Mon, 27 Jul 2009 12:28:53 GMT\r\n"
        "Server: Apache\r\n"
        "Last-Modified: Wed, 22 Jul 2009 19:15:56 GMT\r\n"
        "ETag: \"34aa387-d-1568eb00\"\r\n"
        "Accept-Ranges: bytes\r\n"
        "Content-Length: 52\r\n"
        "Vary: Accept-Encoding\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "Hello World! My payload includes a trailing CRLF.\r\n"
    );
    ASSERT_TRUE(response == nullptr);
}

TEST(ClientTests, ParseIncompleteHeadersBetweenLinesRequest) {
    Http::Client client;
    const auto response = client.ParseResponse(
        "HTTP/1.1 200 OK\r\n"
        "Date: Mon, 27 Jul 2009 12:28:53 GMT\r\n"
        "Server: Apache\r\n"
        "Last-Modified: Wed, 22 Jul 2009 19:15:56 GMT\r\n"
        "ETag: \"34aa387-d-1568eb00\"\r\n"
    );
    ASSERT_TRUE(response == nullptr);
}

TEST(ClientTests, ParseIncompleteHeadersMidLineRequest) {
    Http::Client client;
    const auto response = client.ParseResponse(
        "HTTP/1.1 200 OK\r\n"
        "Date: Mon, 27 Jul 2009 12:28:53 GMT\r\n"
        "Server: Apache\r\n"
        "Last-Modified: Wed, 22 Ju"
    );
    ASSERT_TRUE(response == nullptr);
}

TEST(ClientTests, ParseIncompleteStatusLine) {
    Http::Client client;
    const auto response = client.ParseResponse(
        "HTTP/1.1 200 OK\r"
    );
    ASSERT_TRUE(response == nullptr);
}

TEST(ClientTests, ParseNoHeadersRequest) {
    Http::Client client;
    const auto response = client.ParseResponse(
        "HTTP/1.1 200 OK\r\n"
    );
    ASSERT_TRUE(response == nullptr);
}

TEST(ClientTests, ResponseWithNoContentLengthOrChunkedTransferEncodingHasNoBody) {
    Http::Client client;
    const auto response = client.ParseResponse(
        "HTTP/1.1 200 OK\r\n"
        "Date: Mon, 27 Jul 2009 12:28:53 GMT\r\n"
        "Server: Apache\r\n"
        "Last-Modified: Wed, 22 Jul 2009 19:15:56 GMT\r\n"
        "ETag: \"34aa387-d-1568eb00\"\r\n"
        "Accept-Ranges: bytes\r\n"
        "Vary: Accept-Encoding\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "Hello World! My payload includes a trailing CRLF.\r\n"
    );
    ASSERT_FALSE(response == nullptr);
    ASSERT_EQ("Hello World! My payload includes a trailing CRLF.\r\n", response->body);
}
