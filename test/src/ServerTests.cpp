/**
 * @file ServerTests.cpp
 *
 * This module contains the unit tests of the
 * Http::Server class.
 *
 * © 2018 by Richard Walters
 */

#include <gtest/gtest.h>
#include <Http/Client.hpp>
#include <Http/Server.hpp>
#include <limits>
#include <SystemAbstractions/DiagnosticsSender.hpp>
#include <SystemAbstractions/StringExtensions.hpp>
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
         * This indicates whether or not the mock connection is
         * currently invoking one of the server's delegates.
         */
        bool callingDelegate = false;

        /**
         * This should be held when changing or checking
         * the callingDelegate flag.
         */
        std::recursive_mutex callingDelegateMutex;

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

        // Lifecycle management
        ~MockConnection() {
            std::lock_guard< decltype(callingDelegateMutex) > lock(callingDelegateMutex);
            if (callingDelegate) {
                *((int*)0) = 42; // force a crash (use in a death test)
            }
        }
        MockConnection(const MockConnection&) = delete;
        MockConnection(MockConnection&&) = delete;
        MockConnection& operator=(const MockConnection&) = delete;
        MockConnection& operator=(MockConnection&&) = delete;

        // Methods

        /**
         * This is the constructor for the structure.
         */
        MockConnection() = default;

        // Http::Connection

        virtual std::string GetPeerId() override {
            return "mock-client";
        }

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

/**
 * This is the test fixture for these tests, providing common
 * setup and teardown for each test.
 */
struct ServerTests
    : public ::testing::Test
{
    // Properties

    /**
     * This is the unit under test.
     */
    Http::Server server;

    /**
     * These are the diagnostic messages that have been
     * received from the unit under test.
     */
    std::vector< std::string > diagnosticMessages;

    /**
     * This is the subscription token obtained when subscribing
     * to receive diagnostic messages from the unit under test.
     */
    SystemAbstractions::DiagnosticsSender::SubscriptionToken diagnosticsSubscription;

    // Methods

    // ::testing::Test

    virtual void SetUp() {
        diagnosticsSubscription = server.SubscribeToDiagnostics(
            [this](
                std::string senderName,
                size_t level,
                std::string message
            ){
                diagnosticMessages.push_back(
                    SystemAbstractions::sprintf(
                        "%s[%zu]: %s",
                        senderName.c_str(),
                        level,
                        message.c_str()
                    )
                );
            },
            0
        );
    }

    virtual void TearDown() {
        server.Demobilize();
        server.UnsubscribeFromDiagnostics(diagnosticsSubscription);
    }
};

TEST_F(ServerTests, ParseGetRequest) {
    const auto request = server.ParseRequest(
        "GET /hello.txt HTTP/1.1\r\n"
        "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
        "Host: www.example.com\r\n"
        "Accept-Language: en, mi\r\n"
        "\r\n"
    );
    ASSERT_FALSE(request == nullptr);
    ASSERT_EQ(Http::Request::State::Complete, request->state);
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

TEST_F(ServerTests, ParsePostRequest) {
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
    ASSERT_EQ(Http::Request::State::Complete, request->state);
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

TEST_F(ServerTests, ParseInvalidRequestNoMethod) {
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
    ASSERT_EQ(Http::Request::State::Complete, request->state);
    ASSERT_FALSE(request->valid);
}

TEST_F(ServerTests, ParseInvalidRequestNoTarget) {
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
    ASSERT_EQ(Http::Request::State::Complete, request->state);
    ASSERT_FALSE(request->valid);
}

TEST_F(ServerTests, ParseInvalidRequestBadProtocol) {
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
    ASSERT_EQ(Http::Request::State::Complete, request->state);
    ASSERT_FALSE(request->valid);
}

TEST_F(ServerTests, ParseInvalidDamagedHeader) {
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
    ASSERT_EQ(Http::Request::State::Complete, request->state);
    ASSERT_FALSE(request->valid);
    ASSERT_EQ(rawRequest.length(), messageEnd);
}

TEST_F(ServerTests, ParseInvalidHeaderLineTooLong) {
    size_t messageEnd;
    const std::string testHeaderName("X-Poggers");
    const std::string testHeaderNameWithDelimiters = testHeaderName + ": ";
    const std::string valueIsTooLong(999 - testHeaderNameWithDelimiters.length(), 'X');
    const std::string rawRequest = (
        "GET /hello.txt HTTP/1.1\r\n"
        "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
        + testHeaderNameWithDelimiters + valueIsTooLong + "\r\n"
        "Host: www.example.com\r\n"
        "Accept-Language: en, mi\r\n"
        "\r\n"
    );
    const auto request = server.ParseRequest(rawRequest, messageEnd);
    ASSERT_FALSE(request == nullptr);
    ASSERT_EQ(Http::Request::State::Error, request->state);
}

TEST_F(ServerTests, ParseValidHeaderLineLongerThanDefault) {
    size_t messageEnd;
    const std::string testHeaderName("X-Poggers");
    const std::string testHeaderNameWithDelimiters = testHeaderName + ": ";
    const std::string valueIsLongButWithinCustomLimit(999 - testHeaderNameWithDelimiters.length(), 'X');
    const std::string rawRequest = (
        "GET /hello.txt HTTP/1.1\r\n"
        "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
        + testHeaderNameWithDelimiters + valueIsLongButWithinCustomLimit + "\r\n"
        "Host: www.example.com\r\n"
        "Accept-Language: en, mi\r\n"
        "\r\n"
    );
    ASSERT_EQ("1000", server.GetConfigurationItem("HeaderLineLimit"));
    diagnosticMessages.clear();
    server.SetConfigurationItem("HeaderLineLimit", "1001");
    ASSERT_EQ(
        (std::vector< std::string >{
            "Http::Server[0]: Header line limit changed from 1000 to 1001",
        }),
        diagnosticMessages
    );
    diagnosticMessages.clear();
    ASSERT_EQ("1001", server.GetConfigurationItem("HeaderLineLimit"));
    const auto request = server.ParseRequest(rawRequest, messageEnd);
    ASSERT_FALSE(request == nullptr);
    ASSERT_EQ(Http::Request::State::Complete, request->state);
}

TEST_F(ServerTests, ParseInvalidBodyInsanelyTooLarge) {
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
    ASSERT_EQ(Http::Request::State::Error, request->state);
}

TEST_F(ServerTests, ParseInvalidBodySlightlyTooLarge) {
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
    ASSERT_EQ(Http::Request::State::Error, request->state);
}

TEST_F(ServerTests, ParseIncompleteBodyRequest) {
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

TEST_F(ServerTests, ParseIncompleteHeadersBetweenLinesRequest) {
    size_t messageEnd;
    const std::string rawRequest = (
        "POST / HTTP/1.1\r\n"
        "Host: foo.com\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
    );
    const auto request = server.ParseRequest(rawRequest, messageEnd);
    ASSERT_TRUE(request == nullptr);
}

TEST_F(ServerTests, ParseIncompleteHeadersMidLineRequest) {
    size_t messageEnd;
    const std::string rawRequest = (
        "POST / HTTP/1.1\r\n"
        "Host: foo.com\r\n"
        "Content-Type: application/x-w"
    );
    const auto request = server.ParseRequest(rawRequest, messageEnd);
    ASSERT_TRUE(request == nullptr);
}

TEST_F(ServerTests, ParseIncompleteRequestLine) {
    size_t messageEnd;
    const std::string rawRequest = (
        "POST / HTTP/1.1\r"
    );
    const auto request = server.ParseRequest(rawRequest, messageEnd);
    ASSERT_TRUE(request == nullptr);
}

TEST_F(ServerTests, ParseIncompleteNoHeadersRequest) {
    size_t messageEnd;
    const std::string rawRequest = (
        "POST / HTTP/1.1\r\n"
    );
    const auto request = server.ParseRequest(rawRequest, messageEnd);
    ASSERT_TRUE(request == nullptr);
}

TEST_F(ServerTests, RequestWithNoContentLengthOrChunkedTransferEncodingHasNoBody) {
    const auto request = server.ParseRequest(
        "GET /hello.txt HTTP/1.1\r\n"
        "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
        "Host: www.example.com\r\n"
        "Accept-Language: en, mi\r\n"
        "\r\n"
        "Hello, World!\r\n"
    );
    ASSERT_FALSE(request == nullptr);
    ASSERT_EQ(Http::Request::State::Complete, request->state);
    Uri::Uri expectedUri;
    expectedUri.ParseFromString("/hello.txt");
    ASSERT_TRUE(request->body.empty());
}

TEST_F(ServerTests, Mobilize) {
    auto transport = std::make_shared< MockTransport >();
    ASSERT_TRUE(server.Mobilize(transport, 1234));
    ASSERT_TRUE(transport->bound);
    ASSERT_EQ(1234, transport->port);
    ASSERT_FALSE(transport->connectionDelegate == nullptr);
}

TEST_F(ServerTests, Demobilize) {
    auto transport = std::make_shared< MockTransport >();
    (void)server.Mobilize(transport, 1234);
    server.Demobilize();
    ASSERT_FALSE(transport->bound);
}

TEST_F(ServerTests, ReleaseNetworkUponDestruction) {
    auto transport = std::make_shared< MockTransport >();
    {
        Http::Server temporaryServer;
        (void)temporaryServer.Mobilize(transport, 1234);
    }
    ASSERT_FALSE(transport->bound);
}

TEST_F(ServerTests, ClientRequestInOnePiece) {
    auto transport = std::make_shared< MockTransport >();
    (void)server.Mobilize(transport, 1234);
    ASSERT_EQ(
        (std::vector< std::string >{
            "Http::Server[3]: Now listening on port 1234",
        }),
        diagnosticMessages
    );
    diagnosticMessages.clear();
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    ASSERT_EQ(
        (std::vector< std::string >{
            "Http::Server[2]: New connection from mock-client",
        }),
        diagnosticMessages
    );
    diagnosticMessages.clear();
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
    ASSERT_EQ(
        (std::vector< std::string >{
            "Http::Server[1]: Received GET request for '/hello.txt' from mock-client",
            "Http::Server[1]: Sent 404 'Not Found' response back to mock-client",
        }),
        diagnosticMessages
    );
    diagnosticMessages.clear();
}

TEST_F(ServerTests, ClientRequestInTwoPieces) {
    auto transport = std::make_shared< MockTransport >();
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

TEST_F(ServerTests, TwoClientRequestsInOnePiece) {
    auto transport = std::make_shared< MockTransport >();
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

TEST_F(ServerTests, ClientInvalidRequestRecoverable) {
    auto transport = std::make_shared< MockTransport >();
    (void)server.Mobilize(transport, 1234);
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    ASSERT_FALSE(connection->dataReceivedDelegate == nullptr);
    const std::string requests = (
        "GET /hello.txt HTTP/1.1\r\n"
        "User-Agent curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
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
        "HTTP/1.1 400 Bad Request\r\n"
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
    ASSERT_FALSE(connection->broken);
}

TEST_F(ServerTests, ClientInvalidRequestUnrecoverable) {
    auto transport = std::make_shared< MockTransport >();
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
        "HTTP/1.1 413 Payload Too Large\r\n"
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

TEST_F(ServerTests, ClientConnectionBroken) {
    auto transport = std::make_shared< MockTransport >();
    (void)server.Mobilize(transport, 1234);
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    ASSERT_FALSE(connection->brokenDelegate == nullptr);
    diagnosticMessages.clear();
    connection->brokenDelegate();
    ASSERT_EQ(
        (std::vector< std::string >{
            "Http::Server[2]: Connection to mock-client broken by peer",
        }),
        diagnosticMessages
    );
    diagnosticMessages.clear();
}

TEST_F(ServerTests, ClientShouldNotBeReleasedDuringBreakDelegateCall) {
    auto transport = std::make_shared< MockTransport >();
    (void)server.Mobilize(transport, 1234);
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    auto connectionRaw = connection.get();
    connection = nullptr;
    {
        std::lock_guard< decltype(connectionRaw->callingDelegateMutex) > lock(connectionRaw->callingDelegateMutex);
        connectionRaw->callingDelegate = true;
        connectionRaw->brokenDelegate();
        connectionRaw->callingDelegate = false;
    }
}

TEST_F(ServerTests, ParseInvalidRequestLineTooLong) {
    size_t messageEnd;
    const std::string uriTooLong(1000, 'X');
    const std::string rawRequest = (
        "GET " + uriTooLong + " HTTP/1.1\r\n"
    );
    const auto request = server.ParseRequest(rawRequest, messageEnd);
    ASSERT_FALSE(request == nullptr);
    ASSERT_EQ(Http::Request::State::Error, request->state);
}

TEST_F(ServerTests, ConnectionCloseOrNot) {
    auto transport = std::make_shared< MockTransport >();
    (void)server.Mobilize(transport, 1234);
    for (int i = 0; i < 2; ++i) {
        const auto tellServerToCloseAfterResponse = (i == 0);
        const std::string connectionHeader = (
            tellServerToCloseAfterResponse
            ? "Connection: close\r\n"
            : ""
        );
        auto connection = std::make_shared< MockConnection >();
        transport->connectionDelegate(connection);
        const std::string request = (
            "GET /hello.txt HTTP/1.1\r\n"
            "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
            "Host: www.example.com\r\n"
            "Accept-Language: en, mi\r\n"
            + connectionHeader
            + "\r\n"
        );
        connection->dataReceivedDelegate(
            std::vector< uint8_t >(
                request.begin(),
                request.end()
            )
        );
        if (tellServerToCloseAfterResponse) {
            EXPECT_TRUE(connection->broken) << "We asked the server to close? " << tellServerToCloseAfterResponse;
        } else {
            EXPECT_FALSE(connection->broken) << "We asked the server to close? " << tellServerToCloseAfterResponse;
        }
    }
}

TEST_F(ServerTests, HostMissing) {
    auto transport = std::make_shared< MockTransport >();
    (void)server.Mobilize(transport, 1234);
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    const std::string request = (
        "GET /hello.txt HTTP/1.1\r\n"
        "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
        "Accept-Language: en, mi\r\n"
        "\r\n"
    );
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.end()
        )
    );
    Http::Client client;
    const auto response = client.ParseResponse(
        std::string(
            connection->dataReceived.begin(),
            connection->dataReceived.end()
        )
    );
    ASSERT_FALSE(response == nullptr);
    ASSERT_EQ(400, response->statusCode);
}

TEST_F(ServerTests, HostNotMatchingTargetUri) {
    auto transport = std::make_shared< MockTransport >();
    (void)server.Mobilize(transport, 1234);
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    const std::string request = (
        "GET http://www.example.com/hello.txt HTTP/1.1\r\n"
        "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
        "Host: bad.example.com\r\n"
        "Accept-Language: en, mi\r\n"
        "\r\n"
    );
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.end()
        )
    );
    Http::Client client;
    const auto response = client.ParseResponse(
        std::string(
            connection->dataReceived.begin(),
            connection->dataReceived.end()
        )
    );
    ASSERT_FALSE(response == nullptr);
    ASSERT_EQ(400, response->statusCode);
}

TEST_F(ServerTests, DefaultServerUri) {
    ASSERT_EQ("", server.GetConfigurationItem("Host"));
    const std::vector< std::string > testVectors{
        "www.example.com",
        "bad.example.com",
    };
    size_t index = 0;
    for (const auto& testVector: testVectors) {
        auto transport = std::make_shared< MockTransport >();
        (void)server.Mobilize(transport, 1234);
        auto connection = std::make_shared< MockConnection >();
        transport->connectionDelegate(connection);
        const std::string request = (
            "GET /hello.txt HTTP/1.1\r\n"
            "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
            "Host: " + testVector + "\r\n"
            "Accept-Language: en, mi\r\n"
            "\r\n"
        );
        connection->dataReceivedDelegate(
            std::vector< uint8_t >(
                request.begin(),
                request.end()
            )
        );
        Http::Client client;
        const auto response = client.ParseResponse(
            std::string(
                connection->dataReceived.begin(),
                connection->dataReceived.end()
            )
        );
        ASSERT_FALSE(response == nullptr);
        EXPECT_NE(400, response->statusCode) << "Failed for test vector index " << index;
        ++index;
    }
}

TEST_F(ServerTests, HostNotMatchingServerUri) {
    server.SetConfigurationItem("host", "www.example.com");
    struct TestVector {
        std::string hostHeaderValue;
        bool badRequestStatusExpected;
    };
    const std::vector< TestVector > testVectors{
        { "www.example.com", false },
        { "bad.example.com", true },
    };
    size_t index = 0;
    for (const auto& testVector: testVectors) {
        auto transport = std::make_shared< MockTransport >();
        (void)server.Mobilize(transport, 1234);
        auto connection = std::make_shared< MockConnection >();
        transport->connectionDelegate(connection);
        const std::string request = (
            "GET /hello.txt HTTP/1.1\r\n"
            "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
            "Host: " + testVector.hostHeaderValue + "\r\n"
            "Accept-Language: en, mi\r\n"
            "\r\n"
        );
        connection->dataReceivedDelegate(
            std::vector< uint8_t >(
                request.begin(),
                request.end()
            )
        );
        Http::Client client;
        const auto response = client.ParseResponse(
            std::string(
                connection->dataReceived.begin(),
                connection->dataReceived.end()
            )
        );
        ASSERT_FALSE(response == nullptr);
        if (testVector.badRequestStatusExpected) {
            EXPECT_EQ(400, response->statusCode) << "Failed for test vector index " << index;
        } else {
            EXPECT_NE(400, response->statusCode) << "Failed for test vector index " << index;
        }
        ASSERT_FALSE(connection->broken) << "Failed for test vector index " << index;
        ++index;
    }
}

TEST_F(ServerTests, RegisterResourceDelegateSubspace) {
    auto transport = std::make_shared< MockTransport >();
    (void)server.Mobilize(transport, 1234);
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);

    // Query the server before registering the resource delegate,
    // and expect the canned 404 response since nothing is registered
    // to handle the request yet.
    const std::string request = (
        "GET /foo/bar HTTP/1.1\r\n"
        "Host: www.example.com\r\n"
        "\r\n"
    );
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.end()
        )
    );
    Http::Client client;
    auto response = client.ParseResponse(
        std::string(
            connection->dataReceived.begin(),
            connection->dataReceived.end()
        )
    );
    EXPECT_EQ(404, response->statusCode);
    connection->dataReceived.clear();

    // Register a delegate to handle the source, and then query the server
    // a second time.  Expect the resource request to be routed to the delegate
    // and handled correctly this time.
    std::vector< Uri::Uri > requestsReceived;
    const auto resourceDelegate = [&requestsReceived](
        std::shared_ptr< Http::Request > request
    ){
        const auto response = std::make_shared< Http::Response >();
        response->statusCode = 200;
        response->reasonPhrase = "OK";
        requestsReceived.push_back(request->target);
        return response;
    };
    const auto unregistrationDelegate = server.RegisterResource({ "foo" }, resourceDelegate);
    ASSERT_TRUE(requestsReceived.empty());
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.end()
        )
    );
    response = client.ParseResponse(
        std::string(
            connection->dataReceived.begin(),
            connection->dataReceived.end()
        )
    );
    EXPECT_EQ(200, response->statusCode);
    ASSERT_EQ(1, requestsReceived.size());
    ASSERT_EQ(
        (std::vector< std::string >{ "bar" }),
        requestsReceived[0].GetPath()
    );
    connection->dataReceived.clear();

    // Unregister the resource delegate and then query the server
    // a third time.  Since the resource is no longer registered,
    // we should get the canned 404 response back again.
    unregistrationDelegate();
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.end()
        )
    );
    response = client.ParseResponse(
        std::string(
            connection->dataReceived.begin(),
            connection->dataReceived.end()
        )
    );
    EXPECT_EQ(404, response->statusCode);
    connection->dataReceived.clear();
}

TEST_F(ServerTests, RegisterResourceDelegateServerWide) {
    auto transport = std::make_shared< MockTransport >();
    (void)server.Mobilize(transport, 1234);
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);

    // Query the server before registering the resource delegate,
    // and expect the canned 404 response since nothing is registered
    // to handle the request yet.
    const std::string request = (
        "GET /foo/bar HTTP/1.1\r\n"
        "Host: www.example.com\r\n"
        "\r\n"
    );
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.end()
        )
    );
    Http::Client client;
    auto response = client.ParseResponse(
        std::string(
            connection->dataReceived.begin(),
            connection->dataReceived.end()
        )
    );
    EXPECT_EQ(404, response->statusCode);
    connection->dataReceived.clear();

    // Register a delegate to handle the source, and then query the server
    // a second time.  Expect the resource request to be routed to the delegate
    // and handled correctly this time.
    std::vector< Uri::Uri > requestsReceived;
    const auto resourceDelegate = [&requestsReceived](
        std::shared_ptr< Http::Request > request
    ){
        const auto response = std::make_shared< Http::Response >();
        response->statusCode = 200;
        response->reasonPhrase = "OK";
        requestsReceived.push_back(request->target);
        return response;
    };
    const auto unregistrationDelegate = server.RegisterResource({ }, resourceDelegate);
    ASSERT_TRUE(requestsReceived.empty());
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.end()
        )
    );
    response = client.ParseResponse(
        std::string(
            connection->dataReceived.begin(),
            connection->dataReceived.end()
        )
    );
    EXPECT_EQ(200, response->statusCode);
    ASSERT_EQ(1, requestsReceived.size());
    ASSERT_EQ(
        (std::vector< std::string >{ "foo", "bar" }),
        requestsReceived[0].GetPath()
    );
    connection->dataReceived.clear();

    // Unregister the resource delegate and then query the server
    // a third time.  Since the resource is no longer registered,
    // we should get the canned 404 response back again.
    unregistrationDelegate();
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.end()
        )
    );
    response = client.ParseResponse(
        std::string(
            connection->dataReceived.begin(),
            connection->dataReceived.end()
        )
    );
    EXPECT_EQ(404, response->statusCode);
    connection->dataReceived.clear();
}

TEST_F(ServerTests, DontAllowDoubleRegistration) {
    auto transport = std::make_shared< MockTransport >();
    (void)server.Mobilize(transport, 1234);
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);

    // Register /foo/bar delegate.
    const auto foobar = [](
        std::shared_ptr< Http::Request > request
    ){
        return std::make_shared< Http::Response >();
    };
    const auto unregisterFoobar = server.RegisterResource({ "foo", "bar" }, foobar);

    // Attempt to register another /foo/bar delegate.
    // This should not be allowed because /foo/bar already has a handler.
    const auto imposter = [](
        std::shared_ptr< Http::Request > request
    ){
        return std::make_shared< Http::Response >();
    };
    const auto unregisterImpostor = server.RegisterResource({ "foo", "bar" }, imposter);
    ASSERT_TRUE(unregisterImpostor == nullptr);
}

TEST_F(ServerTests, DontAllowOverlappingSubspaces) {
    auto transport = std::make_shared< MockTransport >();
    (void)server.Mobilize(transport, 1234);
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);

    // Register /foo/bar delegate.
    const auto foobar = [](
        std::shared_ptr< Http::Request > request
    ){
        return std::make_shared< Http::Response >();
    };
    auto unregisterFoobar = server.RegisterResource({ "foo", "bar" }, foobar);
    ASSERT_FALSE(unregisterFoobar == nullptr);

    // Attempt to register /foo delegate.
    // This should not be allowed because it would overlap the /foo/bar delegate.
    const auto foo = [](
        std::shared_ptr< Http::Request > request
    ){
        return std::make_shared< Http::Response >();
    };
    auto unregisterFoo = server.RegisterResource({ "foo" }, foo);
    ASSERT_TRUE(unregisterFoo == nullptr);

    // Unregister /foo/bar and register /foo.
    unregisterFoobar();
    unregisterFoo = server.RegisterResource({ "foo" }, foo);
    ASSERT_FALSE(unregisterFoo == nullptr);

    // Attempt to register /foo/bar again.
    // This should not be allowed because it would overlap the /foo delegate.
    unregisterFoobar = server.RegisterResource({ "foo", "bar" }, foobar);
    ASSERT_TRUE(unregisterFoobar == nullptr);
}

TEST_F(ServerTests, ContentLengthSetByServer) {
    auto transport = std::make_shared< MockTransport >();
    (void)server.Mobilize(transport, 1234);
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    std::vector< Uri::Uri > requestsReceived;
    const auto resourceDelegate = [&requestsReceived](
        std::shared_ptr< Http::Request > request
    ){
        const auto response = std::make_shared< Http::Response >();
        response->statusCode = 200;
        response->reasonPhrase = "OK";
        response->headers.SetHeader("Content-Type", "text/plain");
        response->body = "Hello!";
        requestsReceived.push_back(request->target);
        return response;
    };
    const auto unregistrationDelegate = server.RegisterResource({ "foo" }, resourceDelegate);
    const std::string request = (
        "GET /foo/bar HTTP/1.1\r\n"
        "Host: www.example.com\r\n"
        "\r\n"
    );
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.end()
        )
    );
    Http::Client client;
    const auto response = client.ParseResponse(
        std::string(
            connection->dataReceived.begin(),
            connection->dataReceived.end()
        )
    );
    ASSERT_EQ("6", response->headers.GetHeaderValue("Content-Length"));
}

TEST_F(ServerTests, ClientSentRequestWithTooLargePayloadOverflowingContentLength) {
    auto transport = std::make_shared< MockTransport >();
    (void)server.Mobilize(transport, 1234);
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    std::vector< Uri::Uri > requestsReceived;
    const std::string request = (
        "POST /hello.txt HTTP/1.1\r\n"
        "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
        "Host: www.example.com\r\n"
        "Content-Length: 1000000000000000000000000000000000000000000000000000000000000000000\r\n"
        "Accept-Language: en, mi\r\n"
        "\r\n"
    );
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.end()
        )
    );
    Http::Client client;
    const auto response = client.ParseResponse(
        std::string(
            connection->dataReceived.begin(),
            connection->dataReceived.end()
        )
    );
    EXPECT_EQ(413, response->statusCode);
    EXPECT_EQ("Payload Too Large", response->reasonPhrase);
    EXPECT_TRUE(connection->broken);
}

TEST_F(ServerTests, ClientSentRequestWithTooLargePayloadNotOverflowingContentLength) {
    auto transport = std::make_shared< MockTransport >();
    (void)server.Mobilize(transport, 1234);
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    std::vector< Uri::Uri > requestsReceived;
    const std::string request = (
        "POST /hello.txt HTTP/1.1\r\n"
        "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
        "Host: www.example.com\r\n"
        "Content-Length: 10000000000\r\n"
        "Accept-Language: en, mi\r\n"
        "\r\n"
    );
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.end()
        )
    );
    Http::Client client;
    const auto response = client.ParseResponse(
        std::string(
            connection->dataReceived.begin(),
            connection->dataReceived.end()
        )
    );
    EXPECT_EQ(413, response->statusCode);
    EXPECT_EQ("Payload Too Large", response->reasonPhrase);
    EXPECT_TRUE(connection->broken);
}
