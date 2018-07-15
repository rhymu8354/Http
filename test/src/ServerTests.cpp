/**
 * @file ServerTests.cpp
 *
 * This module contains the unit tests of the
 * Http::Server class.
 *
 * © 2018 by Richard Walters
 */

#include <gtest/gtest.h>
#include <Http/Server.hpp>
#include <limits>
#include <Uri/Uri.hpp>

namespace {

    /**
     * This is a fake client connection which is used to test the server.
     */
    struct MockConnection
        : public Http::Connection
    {
        // Properties

        /**
         * This is the delegate to call whenever data is recevied
         * from the remote peer.
         */
        DataReceivedDelegate dataReceivedDelegate;

        /**
         * This is the delegate to call whenever the connection
         * has been broken.
         */
        BrokenDelegate brokenDelegate;

        /**
         * This holds onto a copy of all data received from the remote peer.
         */
        std::vector< uint8_t > dataReceived;

        /**
         * This flag is set if the remote peer breaks the connection.
         */
        bool broken = false;

        // Methods

        // Http::Connection

        virtual void SetDataReceivedDelegate(DataReceivedDelegate newDataReceivedDelegate) override {
            dataReceivedDelegate = newDataReceivedDelegate;
        }

        virtual void SetBrokenDelegate(BrokenDelegate newBrokenDelegate) override {
            brokenDelegate = newBrokenDelegate;
        }

        virtual void SendData(std::vector< uint8_t > data) override {
            (void)dataReceived.insert(
                dataReceived.end(),
                data.begin(),
                data.end()
            );
        }

        virtual void Break(bool clean) override {
            broken = true;
        }
    };

    /**
     * This is a fake transport layer which is used to test the server.
     */
    struct MockTransport
        : public Http::ServerTransport
    {
        // Properties

        /**
         * This flag indicates whether or not the transport layer
         * has been bound by the server.
         */
        bool bound = false;

        /**
         * This is the port number that the server bound on the transport
         * layer.
         */
        uint16_t port = 0;

        /**
         * This is the delegate to call whenever a new connection
         * has been established for the server.
         */
        NewConnectionDelegate connectionDelegate;

        // Methods

        // Http::ServerTransport

        virtual bool BindNetwork(
            uint16_t newPort,
            NewConnectionDelegate newConnectionDelegate
        ) override {
            port = newPort;
            connectionDelegate = newConnectionDelegate;
            bound = true;
            return true;
        }

        virtual uint16_t GetBoundPort() override {
            return 0;
        }

        virtual void ReleaseNetwork() override {
            bound = false;
        }
    };

}

TEST(ServerTests, ParseGetRequest) {
    Http::Server server;
    const auto request = server.ParseRequest(
        "GET /hello.txt HTTP/1.1\r\n"
        "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
        "Host: www.example.com\r\n"
        "Accept-Language: en, mi\r\n"
        "\r\n"
    );
    ASSERT_FALSE(request == nullptr);
    ASSERT_EQ(Http::Server::Request::Validity::Valid, request->validity);
    Uri::Uri expectedUri;
    expectedUri.ParseFromString("/hello.txt");
    ASSERT_EQ("GET", request->method);
    ASSERT_EQ(expectedUri, request->target);
    ASSERT_TRUE(request->headers.HasHeader("User-Agent"));
    ASSERT_EQ("curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3", request->headers.GetHeaderValue("User-Agent"));
    ASSERT_TRUE(request->headers.HasHeader("Host"));
    ASSERT_EQ("www.example.com", request->headers.GetHeaderValue("Host"));
    ASSERT_TRUE(request->headers.HasHeader("Accept-Language"));
    ASSERT_EQ("en, mi", request->headers.GetHeaderValue("Accept-Language"));
    ASSERT_TRUE(request->body.empty());
}

TEST(ServerTests, ParsePostRequest) {
    Http::Server server;
    size_t messageEnd;
    const std::string rawRequest = (
        "POST / HTTP/1.1\r\n"
        "Host: foo.com\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "say=Hi&to=Mom\r\n"
    );
    const auto request = server.ParseRequest(rawRequest, messageEnd);
    ASSERT_FALSE(request == nullptr);
    ASSERT_EQ(Http::Server::Request::Validity::Valid, request->validity);
    Uri::Uri expectedUri;
    expectedUri.ParseFromString("/");
    ASSERT_EQ("POST", request->method);
    ASSERT_EQ(expectedUri, request->target);
    ASSERT_TRUE(request->headers.HasHeader("Content-Type"));
    ASSERT_EQ("application/x-www-form-urlencoded", request->headers.GetHeaderValue("Content-Type"));
    ASSERT_TRUE(request->headers.HasHeader("Host"));
    ASSERT_EQ("foo.com", request->headers.GetHeaderValue("Host"));
    ASSERT_TRUE(request->headers.HasHeader("Content-Length"));
    ASSERT_EQ("13", request->headers.GetHeaderValue("Content-Length"));
    ASSERT_EQ("say=Hi&to=Mom", request->body);
    ASSERT_EQ(rawRequest.length() - 2, messageEnd);
}

TEST(ServerTests, ParseInvalidRequestNoMethod) {
    Http::Server server;
    size_t messageEnd;
    const std::string rawRequest = (
        " /hello.txt HTTP/1.1\r\n"
        "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
        "Host: www.example.com\r\n"
        "Accept-Language: en, mi\r\n"
        "\r\n"
    );
    const auto request = server.ParseRequest(rawRequest, messageEnd);
    ASSERT_FALSE(request == nullptr);
    ASSERT_EQ(Http::Server::Request::Validity::InvalidRecoverable, request->validity);
}

TEST(ServerTests, ParseInvalidRequestNoTarget) {
    Http::Server server;
    size_t messageEnd;
    const std::string rawRequest = (
        "GET  HTTP/1.1\r\n"
        "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
        "Host: www.example.com\r\n"
        "Accept-Language: en, mi\r\n"
        "\r\n"
    );
    const auto request = server.ParseRequest(rawRequest, messageEnd);
    ASSERT_FALSE(request == nullptr);
    ASSERT_EQ(Http::Server::Request::Validity::InvalidRecoverable, request->validity);
}

TEST(ServerTests, ParseInvalidRequestBadProtocol) {
    Http::Server server;
    size_t messageEnd;
    const std::string rawRequest = (
        "GET /hello.txt Foo\r\n"
        "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
        "Host: www.example.com\r\n"
        "Accept-Language: en, mi\r\n"
        "\r\n"
    );
    const auto request = server.ParseRequest(rawRequest, messageEnd);
    ASSERT_FALSE(request == nullptr);
    ASSERT_EQ(Http::Server::Request::Validity::InvalidRecoverable, request->validity);
}

TEST(ServerTests, ParseInvalidDamagedHeader) {
    Http::Server server;
    size_t messageEnd;
    const std::string rawRequest = (
        "GET /hello.txt HTTP/1.1\r\n"
        "User-Agent curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
        "Host: www.example.com\r\n"
        "Accept-Language: en, mi\r\n"
        "\r\n"
    );
    const auto request = server.ParseRequest(rawRequest, messageEnd);
    ASSERT_FALSE(request == nullptr);
    ASSERT_EQ(Http::Server::Request::Validity::InvalidRecoverable, request->validity);
}

TEST(ServerTests, ParseInvalidHeaderLineTooLong) {
    Http::Server server;
    size_t messageEnd;
    const std::string testHeaderName("X-Poggers");
    const std::string testHeaderNameWithDelimiters = testHeaderName + ": ";
    const std::string valueIsTooLong(999 - testHeaderNameWithDelimiters.length(), 'X');
    const std::string rawRequest = (
        "GET /hello.txt HTTP/1.1\r\n"
        "User-Agent curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
        + testHeaderNameWithDelimiters + valueIsTooLong + "\r\n"
        "Host: www.example.com\r\n"
        "Accept-Language: en, mi\r\n"
        "\r\n"
    );
    const auto request = server.ParseRequest(rawRequest, messageEnd);
    ASSERT_FALSE(request == nullptr);
    ASSERT_EQ(Http::Server::Request::Validity::InvalidRecoverable, request->validity);
}

TEST(ServerTests, ParseInvalidBodyInsanelyTooLarge) {
    Http::Server server;
    const std::string rawRequest = (
        "POST /hello.txt HTTP/1.1\r\n"
        "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
        "Host: www.example.com\r\n"
        "Content-Length: 1000000000000000000000000000000000000000000000000000000000000000000\r\n"
        "Accept-Language: en, mi\r\n"
        "\r\n"
    );
    size_t messageEnd = std::numeric_limits< size_t >::max();
    const auto request = server.ParseRequest(rawRequest, messageEnd);
    ASSERT_FALSE(request == nullptr);
    ASSERT_EQ(Http::Server::Request::Validity::InvalidUnrecoverable, request->validity);
    ASSERT_EQ(0, messageEnd);
}

TEST(ServerTests, ParseInvalidBodySlightlyTooLarge) {
    Http::Server server;
    const std::string rawRequest = (
        "POST /hello.txt HTTP/1.1\r\n"
        "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
        "Host: www.example.com\r\n"
        "Content-Length: 10000001\r\n"
        "Accept-Language: en, mi\r\n"
        "\r\n"
    );
    size_t messageEnd = std::numeric_limits< size_t >::max();
    const auto request = server.ParseRequest(rawRequest, messageEnd);
    ASSERT_FALSE(request == nullptr);
    ASSERT_EQ(Http::Server::Request::Validity::InvalidUnrecoverable, request->validity);
    ASSERT_EQ(0, messageEnd);
}

TEST(ServerTests, ParseIncompleteBodyRequest) {
    Http::Server server;
    size_t messageEnd;
    const std::string rawRequest = (
        "POST / HTTP/1.1\r\n"
        "Host: foo.com\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: 100\r\n"
        "\r\n"
        "say=Hi&to=Mom\r\n"
    );
    const auto request = server.ParseRequest(rawRequest, messageEnd);
    ASSERT_TRUE(request == nullptr);
}

TEST(ServerTests, ParseIncompleteHeadersBetweenLinesRequest) {
    Http::Server server;
    size_t messageEnd;
    const std::string rawRequest = (
        "POST / HTTP/1.1\r\n"
        "Host: foo.com\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
    );
    const auto request = server.ParseRequest(rawRequest, messageEnd);
    ASSERT_TRUE(request == nullptr);
}

TEST(ServerTests, ParseIncompleteHeadersMidLineRequest) {
    Http::Server server;
    size_t messageEnd;
    const std::string rawRequest = (
        "POST / HTTP/1.1\r\n"
        "Host: foo.com\r\n"
        "Content-Type: application/x-w"
    );
    const auto request = server.ParseRequest(rawRequest, messageEnd);
    ASSERT_TRUE(request == nullptr);
}

TEST(ServerTests, ParseIncompleteRequestLine) {
    Http::Server server;
    size_t messageEnd;
    const std::string rawRequest = (
        "POST / HTTP/1.1\r"
    );
    const auto request = server.ParseRequest(rawRequest, messageEnd);
    ASSERT_TRUE(request == nullptr);
}

TEST(ServerTests, ParseIncompleteNoHeadersRequest) {
    Http::Server server;
    size_t messageEnd;
    const std::string rawRequest = (
        "POST / HTTP/1.1\r\n"
    );
    const auto request = server.ParseRequest(rawRequest, messageEnd);
    ASSERT_TRUE(request == nullptr);
}

TEST(ServerTests, RequestWithNoContentLengthOrChunkedTransferEncodingHasNoBody) {
    Http::Server server;
    const auto request = server.ParseRequest(
        "GET /hello.txt HTTP/1.1\r\n"
        "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
        "Host: www.example.com\r\n"
        "Accept-Language: en, mi\r\n"
        "\r\n"
        "Hello, World!\r\n"
    );
    ASSERT_FALSE(request == nullptr);
    ASSERT_EQ(Http::Server::Request::Validity::Valid, request->validity);
    Uri::Uri expectedUri;
    expectedUri.ParseFromString("/hello.txt");
    ASSERT_TRUE(request->body.empty());
}

TEST(ServerTests, Mobilize) {
    auto transport = std::make_shared< MockTransport >();
    Http::Server server;
    ASSERT_TRUE(server.Mobilize(transport, 1234));
    ASSERT_TRUE(transport->bound);
    ASSERT_EQ(1234, transport->port);
    ASSERT_FALSE(transport->connectionDelegate == nullptr);
}

TEST(ServerTests, Demobilize) {
    auto transport = std::make_shared< MockTransport >();
    Http::Server server;
    (void)server.Mobilize(transport, 1234);
    server.Demobilize();
    ASSERT_FALSE(transport->bound);
}

TEST(ServerTests, ReleaseNetworkUponDestruction) {
    auto transport = std::make_shared< MockTransport >();
    {
        Http::Server server;
        (void)server.Mobilize(transport, 1234);
    }
    ASSERT_FALSE(transport->bound);
}

TEST(ServerTests, ClientRequestInOnePiece) {
    auto transport = std::make_shared< MockTransport >();
    Http::Server server;
    (void)server.Mobilize(transport, 1234);
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    ASSERT_FALSE(connection->dataReceivedDelegate == nullptr);
    const std::string request = (
        "GET /hello.txt HTTP/1.1\r\n"
        "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
        "Host: www.example.com\r\n"
        "Accept-Language: en, mi\r\n"
        "\r\n"
    );
    ASSERT_TRUE(connection->dataReceived.empty());
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.end()
        )
    );
    const std::string expectedResponse = (
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Length: 13\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "FeelsBadMan\r\n"
    );
    ASSERT_EQ(
        expectedResponse,
        std::string(
            connection->dataReceived.begin(),
            connection->dataReceived.end()
        )
    );
}

TEST(ServerTests, ClientRequestInTwoPieces) {
    auto transport = std::make_shared< MockTransport >();
    Http::Server server;
    (void)server.Mobilize(transport, 1234);
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    ASSERT_FALSE(connection->dataReceivedDelegate == nullptr);
    const std::string request = (
        "GET /hello.txt HTTP/1.1\r\n"
        "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
        "Host: www.example.com\r\n"
        "Accept-Language: en, mi\r\n"
        "\r\n"
    );
    ASSERT_TRUE(connection->dataReceived.empty());
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.begin() + request.length() / 2
        )
    );
    ASSERT_TRUE(connection->dataReceived.empty());
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin() + request.length() / 2,
            request.end()
        )
    );
    const std::string expectedResponse = (
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Length: 13\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "FeelsBadMan\r\n"
    );
    ASSERT_EQ(
        expectedResponse,
        std::string(
            connection->dataReceived.begin(),
            connection->dataReceived.end()
        )
    );
}

TEST(ServerTests, TwoClientRequestsInOnePiece) {
    auto transport = std::make_shared< MockTransport >();
    Http::Server server;
    (void)server.Mobilize(transport, 1234);
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    ASSERT_FALSE(connection->dataReceivedDelegate == nullptr);
    const std::string requests = (
        "GET /hello.txt HTTP/1.1\r\n"
        "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
        "Host: www.example.com\r\n"
        "Accept-Language: en, mi\r\n"
        "\r\n"
        "GET /hello.txt HTTP/1.1\r\n"
        "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
        "Host: www.example.com\r\n"
        "Accept-Language: en, mi\r\n"
        "\r\n"
    );
    ASSERT_TRUE(connection->dataReceived.empty());
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            requests.begin(),
            requests.end()
        )
    );
    const std::string expectedResponses = (
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Length: 13\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "FeelsBadMan\r\n"
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Length: 13\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "FeelsBadMan\r\n"
    );
    ASSERT_EQ(
        expectedResponses,
        std::string(
            connection->dataReceived.begin(),
            connection->dataReceived.end()
        )
    );
}

TEST(ServerTests, ClientInvalidRequestRecoverable) {
    auto transport = std::make_shared< MockTransport >();
    Http::Server server;
    (void)server.Mobilize(transport, 1234);
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    ASSERT_FALSE(connection->dataReceivedDelegate == nullptr);
    const std::string request = (
        "GET /hello.txt HTTP/1.1\r\n"
        "User-Agent curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
        "Host: www.example.com\r\n"
        "Accept-Language: en, mi\r\n"
        "\r\n"
    );
    ASSERT_TRUE(connection->dataReceived.empty());
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.end()
        )
    );
    const std::string expectedResponse = (
        "HTTP/1.1 400 Bad Request\r\n"
        "Content-Length: 13\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "FeelsBadMan\r\n"
    );
    ASSERT_EQ(
        expectedResponse,
        std::string(
            connection->dataReceived.begin(),
            connection->dataReceived.end()
        )
    );
    ASSERT_FALSE(connection->broken);
}

TEST(ServerTests, ClientInvalidRequestUnrecoverable) {
    auto transport = std::make_shared< MockTransport >();
    Http::Server server;
    (void)server.Mobilize(transport, 1234);
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    ASSERT_FALSE(connection->dataReceivedDelegate == nullptr);
    const std::string request = (
        "POST /hello.txt HTTP/1.1\r\n"
        "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
        "Host: www.example.com\r\n"
        "Content-Length: 100000000000000000000000000000\r\n"
        "Accept-Language: en, mi\r\n"
        "\r\n"
    );
    ASSERT_TRUE(connection->dataReceived.empty());
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.end()
        )
    );
    const std::string expectedResponse = (
        "HTTP/1.1 400 Bad Request\r\n"
        "Content-Length: 13\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "FeelsBadMan\r\n"
    );
    ASSERT_EQ(
        expectedResponse,
        std::string(
            connection->dataReceived.begin(),
            connection->dataReceived.end()
        )
    );
    ASSERT_TRUE(connection->broken);
}
