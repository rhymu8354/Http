/**
 * @file ServerTests.cpp
 *
 * This module contains the unit tests of the
 * Http::Server class.
 *
 * © 2018 by Richard Walters
 */

#include <condition_variable>
#include <future>
#include <gtest/gtest.h>
#include <Http/Client.hpp>
#include <Http/Server.hpp>
#include <inttypes.h>
#include <limits>
#include <StringExtensions/StringExtensions.hpp>
#include <SystemAbstractions/DiagnosticsSender.hpp>
#include <thread>
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
         * This is a function to be called when the mock connection
         * is destroyed.
         */
        std::function< void() > onDestruction;

        /**
         * This is the value to return for the peer address.
         */
        std::string peerAddress = "mock-client";

        /**
         * This is used to synchronize access to the wait condition.
         */
        std::recursive_mutex mutex;

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

        /**
         * This flag indicates whether or not the remote peer broke
         * the connection gracefully.
         */
        bool brokenGracefully = false;

        /**
         * This is used to wait for, or signal, a condition
         * upon which that the tests might be waiting.
         */
        std::condition_variable_any waitCondition;

        // Lifecycle management
        ~MockConnection() noexcept {
            std::lock_guard< decltype(mutex) > lock(mutex);
            if (onDestruction != nullptr) {
                onDestruction();
            }
        }
        MockConnection(const MockConnection&) = delete;
        MockConnection(MockConnection&&) noexcept = delete;
        MockConnection& operator=(const MockConnection&) = delete;
        MockConnection& operator=(MockConnection&&) noexcept = delete;

        // Methods

        /**
         * This is the constructor for the structure.
         */
        MockConnection() = default;

        /**
         * This method waits for the server to return a complete
         * response.
         *
         * @return
         *     An indication of whether or not a complete
         *     response was returned by the server before a reasonable
         *     timeout period has elapsed is returned.
         */
        bool AwaitResponse() {
            std::unique_lock< decltype(mutex) > lock(mutex);
            return waitCondition.wait_for(
                lock,
                std::chrono::milliseconds(100),
                [this]{ return !dataReceived.empty(); }
            );
        }

        /**
         * This method waits for the server to break the conneciton.
         *
         * @return
         *     An indication of whether or not the server broke
         *     the connection is returned.
         */
        bool AwaitBroken() {
            std::unique_lock< decltype(mutex) > lock(mutex);
            return waitCondition.wait_for(
                lock,
                std::chrono::milliseconds(100),
                [this]{ return broken; }
            );
        }

        // Http::Connection

        virtual std::string GetPeerAddress() override {
            return peerAddress;
        }

        virtual std::string GetPeerId() override {
            return "mock-client:5555";
        }

        virtual void SetDataReceivedDelegate(DataReceivedDelegate newDataReceivedDelegate) override {
            dataReceivedDelegate = newDataReceivedDelegate;
        }

        virtual void SetBrokenDelegate(BrokenDelegate newBrokenDelegate) override {
            brokenDelegate = newBrokenDelegate;
        }

        virtual void SendData(const std::vector< uint8_t >& data) override {
            std::lock_guard< decltype(mutex) > lock(mutex);
            (void)dataReceived.insert(
                dataReceived.end(),
                data.begin(),
                data.end()
            );
            waitCondition.notify_all();
        }

        virtual void Break(bool clean) override {
            std::lock_guard< decltype(mutex) > lock(mutex);
            broken = true;
            brokenGracefully = clean;
            waitCondition.notify_all();
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
            if (newPort == 0) {
                port = 1234;
            } else {
                port = newPort;
            }
            connectionDelegate = newConnectionDelegate;
            bound = true;
            return true;
        }

        virtual uint16_t GetBoundPort() override {
            return port;
        }

        virtual void ReleaseNetwork() override {
            bound = false;
        }
    };

    /**
     * This is a fake time-keeper which is used to test the server.
     */
    struct MockTimeKeeper
        : public Http::TimeKeeper
    {
        // Properties

        double currentTime = 0.0;

        // Methods

        // Http::TimeKeeper

        virtual double GetCurrentTime() override {
            return currentTime;
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
     * This is the delegate obtained when subscribing
     * to receive diagnostic messages from the unit under test.
     * It's called to terminate the subscription.
     */
    SystemAbstractions::DiagnosticsSender::UnsubscribeDelegate diagnosticsUnsubscribeDelegate;

    // Methods

    // ::testing::Test

    virtual void SetUp() {
        diagnosticsUnsubscribeDelegate = server.SubscribeToDiagnostics(
            [this](
                std::string senderName,
                size_t level,
                std::string message
            ){
                diagnosticMessages.push_back(
                    StringExtensions::sprintf(
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
        diagnosticsUnsubscribeDelegate();
    }
};

TEST_F(ServerTests, DefaultConfiguration) {
    EXPECT_EQ("1000", server.GetConfigurationItem("HeaderLineLimit"));
    EXPECT_EQ("80", server.GetConfigurationItem("Port"));
    EXPECT_EQ("60.0", server.GetConfigurationItem("RequestTimeout"));
    EXPECT_EQ("60.0", server.GetConfigurationItem("IdleTimeout"));
    EXPECT_EQ("100", server.GetConfigurationItem("BadRequestReportBytes"));
    EXPECT_EQ("60.0", server.GetConfigurationItem("InitialBanPeriod"));
    EXPECT_EQ("60.0", server.GetConfigurationItem("ProbationPeriod"));
    EXPECT_EQ("10.0", server.GetConfigurationItem("TooManyRequestsThreshold"));
    EXPECT_EQ("1.0", server.GetConfigurationItem("TooManyRequestsMeasurementPeriod"));
    EXPECT_EQ("10.0", server.GetConfigurationItem("TooManyRequestsThreshold"));
    EXPECT_EQ("1.0", server.GetConfigurationItem("TooManyRequestsMeasurementPeriod"));
}

TEST_F(ServerTests, ParseGetRequest_ASCII_Target_URI) {
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

TEST_F(ServerTests, ParseGetRequest_Non_ASCII_Target_URI) {
    const auto request = server.ParseRequest(
        "GET /%F0%9F%92%A9.txt HTTP/1.1\r\n"
        "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
        "Host: www.example.com\r\n"
        "Accept-Language: en, mi\r\n"
        "\r\n"
    );
    ASSERT_FALSE(request == nullptr);
    ASSERT_EQ(Http::Request::State::Complete, request->state);
    Uri::Uri expectedUri;
    expectedUri.SetPath({"", "💩.txt"});
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

TEST_F(ServerTests, ParseInvalidRequestNoProtocol) {
    size_t messageEnd;
    const std::string rawRequest = (
        "GET /hello.txt\r\n"
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

TEST_F(ServerTests, MobilizeKnownPort) {
    auto transport = std::make_shared< MockTransport >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    ASSERT_TRUE(server.Mobilize(deps));
    ASSERT_TRUE(transport->bound);
    ASSERT_EQ(1234, transport->port);
    ASSERT_FALSE(transport->connectionDelegate == nullptr);
}

TEST_F(ServerTests, MobilizeRandomPort) {
    auto transport = std::make_shared< MockTransport >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    EXPECT_EQ("80", server.GetConfigurationItem("Port"));
    server.SetConfigurationItem("Port", "0");
    diagnosticMessages.clear();
    ASSERT_TRUE(server.Mobilize(deps));
    ASSERT_TRUE(transport->bound);
    EXPECT_EQ(1234, transport->port);
    EXPECT_EQ("1234", server.GetConfigurationItem("Port"));
    ASSERT_FALSE(transport->connectionDelegate == nullptr);
    ASSERT_EQ(
        (std::vector< std::string >{
            "Http::Server[3]: Now listening on port 1234"
        }),
        diagnosticMessages
    );
}

TEST_F(ServerTests, Demobilize) {
    auto transport = std::make_shared< MockTransport >();
    bool timeKeeperReleased = false;
    {
        Http::Server::MobilizationDependencies deps;
        deps.transport = transport;
        deps.timeKeeper = std::shared_ptr< MockTimeKeeper >(
            new MockTimeKeeper(),
            [&timeKeeperReleased](MockTimeKeeper* p) {
                delete p;
                timeKeeperReleased = true;
            }
        );
        server.SetConfigurationItem("Port", "1234");
        (void)server.Mobilize(deps);
        server.Demobilize();
    }
    ASSERT_TRUE(timeKeeperReleased);
    ASSERT_FALSE(transport->bound);
}

TEST_F(ServerTests, ReleaseNetworkUponDestruction) {
    auto transport = std::make_shared< MockTransport >();
    {
        Http::Server temporaryServer;
        Http::Server::MobilizationDependencies deps;
        deps.transport = transport;
        deps.timeKeeper = std::make_shared< MockTimeKeeper >();
        server.SetConfigurationItem("Port", "1234");
        (void)temporaryServer.Mobilize(deps);
    }
    ASSERT_FALSE(transport->bound);
}

TEST_F(ServerTests, ClientRequestInOnePiece) {
    auto transport = std::make_shared< MockTransport >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    (void)server.Mobilize(deps);
    ASSERT_EQ(
        (std::vector< std::string >{
            "Http::Server[0]: Port number changed from 80 to 1234",
            "Http::Server[3]: Now listening on port 1234",
        }),
        diagnosticMessages
    );
    diagnosticMessages.clear();
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    ASSERT_EQ(
        (std::vector< std::string >{
            "Http::Server[2]: New connection from mock-client:5555",
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
        "Content-Type: text/plain\r\n"
        "Content-Length: 13\r\n"
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

TEST_F(ServerTests, ClientRequestInTwoPieces) {
    auto transport = std::make_shared< MockTransport >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    (void)server.Mobilize(deps);
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
        "Content-Type: text/plain\r\n"
        "Content-Length: 13\r\n"
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
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    (void)server.Mobilize(deps);
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
        "Content-Type: text/plain\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "FeelsBadMan\r\n"
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 13\r\n"
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

TEST_F(ServerTests, ClientInvalidRequest) {
    auto transport = std::make_shared< MockTransport >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    (void)server.Mobilize(deps);
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
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "Content-Length: 13\r\n"
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
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    (void)server.Mobilize(deps);
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    ASSERT_FALSE(connection->brokenDelegate == nullptr);
    diagnosticMessages.clear();
    connection->brokenDelegate(false);
    ASSERT_EQ(
        (std::vector< std::string >{
            "Http::Server[2]: Connection to mock-client:5555 broken by peer",
        }),
        diagnosticMessages
    );
    diagnosticMessages.clear();
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
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    (void)server.Mobilize(deps);
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
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    (void)server.Mobilize(deps);
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
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    (void)server.Mobilize(deps);
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
        Http::Server::MobilizationDependencies deps;
        deps.transport = transport;
        deps.timeKeeper = std::make_shared< MockTimeKeeper >();
        server.SetConfigurationItem("Port", "1234");
        (void)server.Mobilize(deps);
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
        server.Demobilize();
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
        Http::Server::MobilizationDependencies deps;
        deps.transport = transport;
        deps.timeKeeper = std::make_shared< MockTimeKeeper >();
        server.SetConfigurationItem("Port", "1234");
        (void)server.Mobilize(deps);
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
            ASSERT_TRUE(connection->broken) << "Failed for test vector index " << index;
        } else {
            EXPECT_NE(400, response->statusCode) << "Failed for test vector index " << index;
            ASSERT_FALSE(connection->broken) << "Failed for test vector index " << index;
        }
        server.Demobilize();
        ++index;
    }
}

TEST_F(ServerTests, RegisterResourceDelegateSubspace) {
    auto transport = std::make_shared< MockTransport >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    (void)server.Mobilize(deps);
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
        const Http::Request& request,
        std::shared_ptr< Http::Connection > connection,
        const std::string& trailer
    ){
        Http::Response response;
        response.statusCode = 200;
        response.reasonPhrase = "OK";
        requestsReceived.push_back(request.target);
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
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    (void)server.Mobilize(deps);
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
        const Http::Request& request,
        std::shared_ptr< Http::Connection > connection,
        const std::string& trailer
    ){
        Http::Response response;
        response.statusCode = 200;
        response.reasonPhrase = "OK";
        requestsReceived.push_back(request.target);
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

TEST_F(ServerTests, UnRegisterResourceDelegateServerWideWhenSubspaceStillRegistered) {
    // Setup
    auto transport = std::make_shared< MockTransport >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    (void)server.Mobilize(deps);
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);

    // Register server-wide resource delegate.
    const auto resourceDelegate = [](
        const Http::Request& request,
        std::shared_ptr< Http::Connection > connection,
        const std::string& trailer
    ){
        Http::Response response;
        response.statusCode = 200;
        response.reasonPhrase = "OK";
        return response;
    };
    const auto topUnregistrationDelegate = server.RegisterResource({ }, resourceDelegate);

    // Register a subspace resource delegate.
    const auto subspaceUnregistrationDelegate = server.RegisterResource({ "foo" }, resourceDelegate);

    // Now try to unregister the server-wide resource delegate, to make sure
    // this does not crash (which it did at one point due to a bug).
    topUnregistrationDelegate();

    // Query the server for the subspace resource, to make sure it still works.
    const std::string request = (
        "GET /foo HTTP/1.1\r\n"
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
    EXPECT_EQ(200, response->statusCode);
}

TEST_F(ServerTests, DontAllowDoubleRegistration) {
    auto transport = std::make_shared< MockTransport >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    (void)server.Mobilize(deps);
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);

    // Register /foo/bar delegate.
    const auto foobar = [](
        const Http::Request& request,
        std::shared_ptr< Http::Connection > connection,
        const std::string& trailer
    ){
        return Http::Response();
    };
    const auto unregisterFoobar = server.RegisterResource({ "foo", "bar" }, foobar);

    // Attempt to register another /foo/bar delegate.
    // This should not be allowed because /foo/bar already has a handler.
    const auto imposter = [](
        const Http::Request& request,
        std::shared_ptr< Http::Connection > connection,
        const std::string& trailer
    ){
        return Http::Response();
    };
    const auto unregisterImpostor = server.RegisterResource({ "foo", "bar" }, imposter);
    ASSERT_TRUE(unregisterImpostor == nullptr);
}

TEST_F(ServerTests, DoAllowOverlappingSubspaces) {
    auto transport = std::make_shared< MockTransport >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    (void)server.Mobilize(deps);
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);

    // Register /foo/bar delegate.
    bool fooBarAccessed = false;
    const auto foobar = [&fooBarAccessed](
        const Http::Request& request,
        std::shared_ptr< Http::Connection > connection,
        const std::string& trailer
    ){
        fooBarAccessed = true;
        return Http::Response();
    };
    auto unregisterFoobar = server.RegisterResource({ "foo", "bar" }, foobar);
    ASSERT_FALSE(unregisterFoobar == nullptr);

    // Attempt to register /foo delegate.
    // This should be allowed because although it would overlap the /foo/bar
    // space, it doesn't exactly match it.
    bool fooAccessed = false;
    const auto foo = [&fooAccessed](
        const Http::Request& request,
        std::shared_ptr< Http::Connection > connection,
        const std::string& trailer
    ){
        fooAccessed = true;
        return Http::Response();
    };
    auto unregisterFoo = server.RegisterResource({ "foo" }, foo);
    ASSERT_FALSE(unregisterFoo == nullptr);

    // Test that queries to the two overlapping subspaces are routed
    // to the correct handlers.
    const std::string fooRequest = (
        "GET /foo/index.html HTTP/1.1\r\n"
        "Host: www.example.com\r\n"
        "\r\n"
    );
    const std::string fooBarRequest = (
        "GET /foo/bar/index.html HTTP/1.1\r\n"
        "Host: www.example.com\r\n"
        "\r\n"
    );
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            fooRequest.begin(),
            fooRequest.end()
        )
    );
    EXPECT_TRUE(fooAccessed);
    EXPECT_FALSE(fooBarAccessed);
    fooAccessed = fooBarAccessed = false;
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            fooBarRequest.begin(),
            fooBarRequest.end()
        )
    );
    EXPECT_FALSE(fooAccessed);
    EXPECT_TRUE(fooBarAccessed);
    fooAccessed = fooBarAccessed = false;

    // Unregister the /foo/bar delegate, and send a new request to /foo/bar,
    // this time expecting the /foo delegate to be called.
    unregisterFoobar();
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            fooBarRequest.begin(),
            fooBarRequest.end()
        )
    );
    EXPECT_TRUE(fooAccessed);
    EXPECT_FALSE(fooBarAccessed);
}

TEST_F(ServerTests, ContentLengthSetByServer) {
    auto transport = std::make_shared< MockTransport >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    (void)server.Mobilize(deps);
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    std::vector< Uri::Uri > requestsReceived;
    const auto resourceDelegate = [&requestsReceived](
        const Http::Request& request,
        std::shared_ptr< Http::Connection > connection,
        const std::string& trailer
    ){
        Http::Response response;
        response.statusCode = 200;
        response.reasonPhrase = "OK";
        response.headers.SetHeader("Content-Type", "text/plain");
        response.body = "Hello!";
        requestsReceived.push_back(request.target);
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
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    (void)server.Mobilize(deps);
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
    EXPECT_TRUE(response->headers.HasHeaderToken("Connection", "close"));
    EXPECT_TRUE(connection->broken);
    EXPECT_TRUE(connection->brokenGracefully);
}

TEST_F(ServerTests, ClientSentRequestWithTooLargePayloadNotOverflowingContentLength) {
    auto transport = std::make_shared< MockTransport >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    (void)server.Mobilize(deps);
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
    EXPECT_TRUE(response->headers.HasHeaderToken("Connection", "close"));
    EXPECT_TRUE(connection->broken);
    EXPECT_TRUE(connection->brokenGracefully);
}

TEST_F(ServerTests, InactivityTimeout) {
    const auto transport = std::make_shared< MockTransport >();
    const auto timeKeeper = std::make_shared< MockTimeKeeper >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = timeKeeper;
    server.SetConfigurationItem("Port", "1234");
    server.SetConfigurationItem("InactivityTimeout", "1.0");
    (void)server.Mobilize(deps);
    auto& scheduler = server.GetScheduler();
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    const std::string request = (
        "GET /foo/bar HTTP/1.1\r\n"
        "Host: www.example.com\r\n"
    );
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.end()
        )
    );
    timeKeeper->currentTime = 0.999;
    scheduler.WakeUp();
    ASSERT_FALSE(connection->AwaitResponse());
    connection->dataReceivedDelegate({'x'});
    timeKeeper->currentTime = 1.001;
    scheduler.WakeUp();
    ASSERT_FALSE(connection->AwaitResponse());
    timeKeeper->currentTime = 1.998;
    scheduler.WakeUp();
    ASSERT_FALSE(connection->AwaitResponse());
    timeKeeper->currentTime = 2.000;
    scheduler.WakeUp();
    ASSERT_TRUE(connection->AwaitResponse());
    Http::Client client;
    std::string dataReceived(
        connection->dataReceived.begin(),
        connection->dataReceived.end()
    );
    const auto response = client.ParseResponse(
        dataReceived
    );
    EXPECT_EQ(408, response->statusCode);
    EXPECT_EQ("Request Timeout", response->reasonPhrase);
}

TEST_F(ServerTests, RequestTimeout) {
    const auto transport = std::make_shared< MockTransport >();
    const auto timeKeeper = std::make_shared< MockTimeKeeper >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = timeKeeper;
    server.SetConfigurationItem("Port", "1234");
    server.SetConfigurationItem("InactivityTimeout", "10.0");
    server.SetConfigurationItem("RequestTimeout", "1.0");
    (void)server.Mobilize(deps);
    auto& scheduler = server.GetScheduler();
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    const std::string request = (
        "GET /foo/bar HTTP/1.1\r\n"
        "Host: www.example.com\r\n"
    );
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.end()
        )
    );
    timeKeeper->currentTime = 0.999;
    scheduler.WakeUp();
    ASSERT_FALSE(connection->AwaitResponse());
    // connection->dataReceivedDelegate({'x'});
    timeKeeper->currentTime = 1.001;
    scheduler.WakeUp();
    ASSERT_TRUE(connection->AwaitResponse());
    Http::Client client;
    const auto response = client.ParseResponse(
        std::string(
            connection->dataReceived.begin(),
            connection->dataReceived.end()
        )
    );
    EXPECT_EQ(408, response->statusCode);
    EXPECT_EQ("Request Timeout", response->reasonPhrase);
    ASSERT_TRUE(connection->AwaitBroken());
    connection->dataReceived.clear();
    timeKeeper->currentTime = 1.001;
    scheduler.WakeUp();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_TRUE(connection->dataReceived.empty());
}

TEST_F(ServerTests, MobilizeWhenAlreadyMobilized) {
    Http::Server::MobilizationDependencies deps;
    deps.transport = std::make_shared< MockTransport >();
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    ASSERT_TRUE(server.Mobilize(deps));
    ASSERT_FALSE(server.Mobilize(deps));
}

TEST_F(ServerTests, IdleTimeout) {
    const auto transport = std::make_shared< MockTransport >();
    const auto timeKeeper = std::make_shared< MockTimeKeeper >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = timeKeeper;
    server.SetConfigurationItem("InactivityTimeout", "10.0");
    server.SetConfigurationItem("RequestTimeout", "1.0");
    server.SetConfigurationItem("IdleTimeout", "100.0");
    (void)server.Mobilize(deps);
    auto& scheduler = server.GetScheduler();
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    timeKeeper->currentTime = 1.001;
    scheduler.WakeUp();
    ASSERT_FALSE(connection->AwaitBroken());
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
    ASSERT_TRUE(connection->AwaitResponse());
    connection->dataReceived.clear();
    timeKeeper->currentTime = 2.002;
    scheduler.WakeUp();
    ASSERT_FALSE(connection->AwaitBroken());
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.end()
        )
    );
    ASSERT_TRUE(connection->AwaitResponse());
    timeKeeper->currentTime = 30.0;
    scheduler.WakeUp();
    ASSERT_FALSE(connection->AwaitBroken());
    timeKeeper->currentTime = 103.0;
    scheduler.WakeUp();
    ASSERT_TRUE(connection->AwaitBroken());
}

TEST_F(ServerTests, NoDiagnosticMessageIfConfigurationItemDidNotReallyChange) {
    server.SetConfigurationItem("HeaderLineLimit", "1000");
    server.SetConfigurationItem("HeaderLineLimit", "1001");
    server.SetConfigurationItem("Port", "80");
    server.SetConfigurationItem("Port", "81");
    server.SetConfigurationItem("InactivityTimeout", "1.0");
    server.SetConfigurationItem("InactivityTimeout", "1.1");
    server.SetConfigurationItem("RequestTimeout", "60.0");
    server.SetConfigurationItem("RequestTimeout", "60.1");
    server.SetConfigurationItem("IdleTimeout", "60.0");
    server.SetConfigurationItem("IdleTimeout", "60.1");
    ASSERT_EQ(
        (std::vector< std::string >{
            "Http::Server[0]: Header line limit changed from 1000 to 1001",
            "Http::Server[0]: Port number changed from 80 to 81",
            "Http::Server[0]: Inactivity timeout changed from 1.000000 to 1.100000",
            "Http::Server[0]: Request timeout changed from 60.000000 to 60.100000",
            "Http::Server[0]: Idle timeout changed from 60.000000 to 60.100000",
        }),
        diagnosticMessages
    );
}

TEST_F(ServerTests, UpgradeConnection) {
    // Set up the server.
    auto transport = std::make_shared< MockTransport >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    (void)server.Mobilize(deps);

    // Register resource handler that will upgrade the connection
    // of any request sent to it.
    bool requestReceived = false;
    std::shared_ptr< Http::Connection > upgradedConnection;
    std::string dataReceivedAfterUpgrading;
    const auto resourceDelegate = [
        &upgradedConnection,
        &requestReceived,
        &dataReceivedAfterUpgrading
    ](
        const Http::Request& request,
        std::shared_ptr< Http::Connection > connection,
        const std::string& trailer
    ){
        requestReceived = true;
        Http::Response response;
        response.statusCode = 101;
        response.reasonPhrase = "Switching Protocols";
        response.headers.SetHeader("Connection", "upgrade");
        upgradedConnection = connection;
        dataReceivedAfterUpgrading = trailer;
        upgradedConnection->SetDataReceivedDelegate(
            [&dataReceivedAfterUpgrading](std::vector< uint8_t > data){
                dataReceivedAfterUpgrading += std::string(
                    data.begin(),
                    data.end()
                );
            }
        );
        upgradedConnection->SetBrokenDelegate(
            [](bool){}
        );
        return response;
    };
    const auto unregistrationDelegate = server.RegisterResource({ "foo" }, resourceDelegate);

    // Connect to server.
    auto connection = std::make_shared< MockConnection >();
    bool connectionDestroyed = false;
    connection->onDestruction = [&connectionDestroyed]{ connectionDestroyed = true; };
    transport->connectionDelegate(connection);

    // Send a request that should trigger an upgrade.
    const std::string request = (
        "GET /foo/bar HTTP/1.1\r\n"
        "Host: www.example.com\r\n"
        "\r\n"
        "Hello!\r\n"
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
    connection->dataReceived.clear();
    EXPECT_TRUE(requestReceived);
    EXPECT_EQ(101, response->statusCode);
    ASSERT_EQ(connection, upgradedConnection);
    EXPECT_EQ("Hello!\r\n", dataReceivedAfterUpgrading);
    dataReceivedAfterUpgrading.clear();

    // Send the request again.  This time the request should not
    // be routed to the resource handler, because the server should
    // have relayed the connection to the resource handler.
    requestReceived = false;
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.end()
        )
    );
    EXPECT_TRUE(connection->dataReceived.empty());
    EXPECT_FALSE(connection->broken);
    EXPECT_FALSE(requestReceived);
    EXPECT_EQ(request, dataReceivedAfterUpgrading);

    // Release the upgraded connection.  That should be the last
    // reference to the connection, so expect it to have been destroyed.
    connection = nullptr;
    upgradedConnection = nullptr;
    ASSERT_TRUE(connectionDestroyed);
}

TEST_F(ServerTests, UpgradedConnectionsShouldNotBeTimedOutByServer) {
    // Set up the server.
    auto transport = std::make_shared< MockTransport >();
    const auto timeKeeper = std::make_shared< MockTimeKeeper >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = timeKeeper;
    server.SetConfigurationItem("Port", "1234");
    server.SetConfigurationItem("RequestTimeout", "1.0");
    (void)server.Mobilize(deps);

    // Register resource handler that will upgrade the connection
    // of any request sent to it.
    bool requestReceived = false;
    std::shared_ptr< Http::Connection > upgradedConnection;
    const auto resourceDelegate = [
        &upgradedConnection,
        &requestReceived
    ](
        const Http::Request& request,
        std::shared_ptr< Http::Connection > connection,
        const std::string& trailer
    ){
        requestReceived = true;
        Http::Response response;
        response.statusCode = 101;
        response.reasonPhrase = "Switching Protocols";
        response.headers.SetHeader("Connection", "upgrade");
        upgradedConnection = connection;
        upgradedConnection->SetDataReceivedDelegate(
            [](std::vector< uint8_t > data){}
        );
        upgradedConnection->SetBrokenDelegate(
            [](bool){}
        );
        return response;
    };
    const auto unregistrationDelegate = server.RegisterResource({ "foo" }, resourceDelegate);

    // Connect to server.
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);

    // Send a request that should trigger an upgrade.
    const std::string request = (
        "GET /foo/bar HTTP/1.1\r\n"
        "Host: www.example.com\r\n"
        "\r\n"
        "Hello!\r\n"
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
    connection->dataReceived.clear();
    timeKeeper->currentTime = 1.001;
    ASSERT_FALSE(connection->AwaitResponse());
}

TEST_F(ServerTests, UpgradedConnectionShouldNoLongerParseRequests) {
    // Set up the server.
    auto transport = std::make_shared< MockTransport >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    (void)server.Mobilize(deps);

    // Register resource handler that will upgrade the connection
    // of any request sent to it.
    size_t requestsReceived = 0;
    std::shared_ptr< Http::Connection > upgradedConnection;
    std::string dataReceivedAfterUpgrading;
    const auto resourceDelegate = [
        &upgradedConnection,
        &requestsReceived,
        &dataReceivedAfterUpgrading
    ](
        const Http::Request& request,
        std::shared_ptr< Http::Connection > connection,
        const std::string& trailer
    ){
        ++requestsReceived;
        Http::Response response;
        response.statusCode = 101;
        response.reasonPhrase = "Switching Protocols";
        response.headers.SetHeader("Connection", "upgrade");
        upgradedConnection = connection;
        dataReceivedAfterUpgrading = trailer;
        upgradedConnection->SetDataReceivedDelegate(
            [&dataReceivedAfterUpgrading](std::vector< uint8_t > data){
                dataReceivedAfterUpgrading += std::string(
                    data.begin(),
                    data.end()
                );
            }
        );
        upgradedConnection->SetBrokenDelegate(
            [](bool){}
        );
        return response;
    };
    const auto unregistrationDelegate = server.RegisterResource({ "foo" }, resourceDelegate);

    // Connect to server.
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);

    // Send a request that should trigger an upgrade, but make the data after
    // the request look like another request.
    const std::string request = (
        "GET /foo/bar HTTP/1.1\r\n"
        "Host: www.example.com\r\n"
        "\r\n"
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
    connection->dataReceived.clear();
    EXPECT_EQ(1, requestsReceived);
    EXPECT_EQ(101, response->statusCode);
    ASSERT_EQ(connection, upgradedConnection);
    EXPECT_EQ(
        "GET /foo/bar HTTP/1.1\r\n"
        "Host: www.example.com\r\n"
        "\r\n",
        dataReceivedAfterUpgrading
    );
    dataReceivedAfterUpgrading.clear();
}

TEST_F(ServerTests, GoodRequestDiagnosticMessage) {
    // Set up server.
    auto transport = std::make_shared< MockTransport >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    (void)server.Mobilize(deps);

    // Register a mock resource delegate.
    std::vector< Uri::Uri > requestsReceived;
    const auto resourceDelegate = [&requestsReceived](
        const Http::Request& request,
        std::shared_ptr< Http::Connection > connection,
        const std::string& trailer
    ){
        Http::Response response;
        response.statusCode = 200;
        response.reasonPhrase = "OK";
        response.body = "Hello!";
        response.headers.SetHeader("Content-Type", "text/plain");
        requestsReceived.push_back(request.target);
        return response;
    };
    const auto unregistrationDelegate = server.RegisterResource({ "foo" }, resourceDelegate);
    ASSERT_TRUE(requestsReceived.empty());

    // Start a new mock connection.
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);

    // Issue a good GET request to registered resource.
    std::string request = (
        "GET /foo/bar HTTP/1.1\r\n"
        "Host: www.example.com\r\n"
        "\r\n"
    );
    diagnosticMessages.clear();
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
    ASSERT_EQ(
        (std::vector< std::string >{
            "Http::Server[1]: Request: GET '/foo/bar' (0) from mock-client:5555: 200 (text/plain:6)",
        }),
        diagnosticMessages
    );
    diagnosticMessages.clear();

    // Issue a good POST request to registered resource.
    request = (
        "POST /foo/spam HTTP/1.1\r\n"
        "Host: www.example.com\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 8\r\n"
        "\r\n"
        "PogChamp"
    );
    diagnosticMessages.clear();
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
    ASSERT_EQ(
        (std::vector< std::string >{
            "Http::Server[1]: Request: POST '/foo/spam' (text/plain:8) from mock-client:5555: 200 (text/plain:6)",
        }),
        diagnosticMessages
    );
    diagnosticMessages.clear();

    // Issue a good GET request to an unregistered resource.
    request = (
        "GET /nowhere HTTP/1.1\r\n"
        "Host: www.example.com\r\n"
        "\r\n"
    );
    diagnosticMessages.clear();
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
    ASSERT_EQ(
        (std::vector< std::string >{
            "Http::Server[1]: Request: GET '/nowhere' (0) from mock-client:5555: 404 (text/plain:13)",
        }),
        diagnosticMessages
    );
    diagnosticMessages.clear();
}

TEST_F(ServerTests, BadRequestDiagnosticMessage) {
    // Set up server.
    auto transport = std::make_shared< MockTransport >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    server.SetConfigurationItem("BadRequestReportBytes", "10");
    (void)server.Mobilize(deps);

    // Start a new mock connection.
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);

    // Issue a bad request.
    std::string request(
        "Pog\0Champ This is a baaaaaaad request!\r\n\r\n",
        42
    );
    diagnosticMessages.clear();
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.end()
        )
    );
    ASSERT_EQ(
        (std::vector< std::string >{
            "Http::Server[3]: Request: Bad request from mock-client:5555: Pog\\x00Champ\\x20",
            "Http::Server[3]: Request: mock-client banned for 60 seconds (Bad HTTP: 400 Bad Request)",
            "Http::Server[2]: Connection to mock-client:5555 closed by server",
        }),
        diagnosticMessages
    );
}

TEST_F(ServerTests, BadRequestResultsInBan) {
    // Set up server.
    auto transport = std::make_shared< MockTransport >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    (void)server.Mobilize(deps);

    // Start a new mock connection.
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);

    // Issue a bad request.
    const std::string request(
        "Pog\0Champ This is a baaaaaaad request!\r\n\r\n",
        42
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
    ASSERT_FALSE(response == nullptr);
    EXPECT_EQ(400, response->statusCode);
    EXPECT_TRUE(connection->broken);
    EXPECT_TRUE(connection->brokenGracefully);

    // Start a second mock connection.
    connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);

    // Issue another bad request.
    EXPECT_TRUE(connection->dataReceivedDelegate == nullptr);
    EXPECT_TRUE(connection->broken);
    EXPECT_FALSE(connection->brokenGracefully);
}

TEST_F(ServerTests, GoodRequestAcceptedWhileOnProbation) {
    // Set up server.
    auto transport = std::make_shared< MockTransport >();
    const auto timeKeeper = std::make_shared< MockTimeKeeper >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = timeKeeper;
    server.SetConfigurationItem("Port", "1234");
    server.SetConfigurationItem("InitialBanPeriod", "1.0");
    (void)server.Mobilize(deps);

    // Start a new mock connection.
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);

    // Issue a bad request.
    std::string request(
        "Pog\0Champ This is a baaaaaaad request!\r\n\r\n",
        42
    );
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.end()
        )
    );
    connection = nullptr;

    // Advance time until the client is on probation.
    timeKeeper->currentTime = 1.5;

    // Start a second mock connection.
    connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    ASSERT_FALSE(connection->dataReceivedDelegate == nullptr);

    // Issue a good request and expect some response.
    request = (
        "GET / HTTP/1.1\r\n"
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
    ASSERT_FALSE(response == nullptr);
    EXPECT_FALSE(connection->broken);
}

TEST_F(ServerTests, BadRequestWhileOnProbationExtendsBan) {
    // Set up server.
    auto transport = std::make_shared< MockTransport >();
    const auto timeKeeper = std::make_shared< MockTimeKeeper >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = timeKeeper;
    server.SetConfigurationItem("Port", "1234");
    server.SetConfigurationItem("InitialBanPeriod", "1.0");
    server.SetConfigurationItem("BadRequestReportBytes", "10");
    (void)server.Mobilize(deps);
    auto& schedule = server.GetScheduler();

    // Start a new mock connection.
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);

    // Issue a bad request.
    std::string request(
        "Pog\0Champ This is a baaaaaaad request!\r\n\r\n",
        42
    );
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.end()
        )
    );
    ASSERT_TRUE(connection->AwaitBroken());
    connection->broken = false;
    connection->brokenDelegate(true);
    ASSERT_TRUE(connection->AwaitBroken());
    connection = nullptr;

    // Advance time until the client is on probation.
    timeKeeper->currentTime = 1.5;
    schedule.WakeUp();

    // Start a second mock connection.
    connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    ASSERT_FALSE(connection->dataReceivedDelegate == nullptr);

    // Issue bad request again.
    diagnosticMessages.clear();
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.end()
        )
    );
    EXPECT_TRUE(connection->broken);
    EXPECT_TRUE(connection->brokenGracefully);
    connection = nullptr;
    ASSERT_EQ(
        (std::vector< std::string >{
            "Http::Server[3]: Request: Bad request from mock-client:5555: Pog\\x00Champ\\x20",
            "Http::Server[3]: Request: mock-client ban extended to 2 seconds (Bad HTTP: 400 Bad Request)",
            "Http::Server[2]: Connection to mock-client:5555 closed by server",
        }),
        diagnosticMessages
    );

    // Advance time one more initial ban period.
    timeKeeper->currentTime = 2.5;
    schedule.WakeUp();

    // Connect a third time, but expect to be still banned.
    connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    EXPECT_TRUE(connection->dataReceivedDelegate == nullptr);

    // Advance time one more initial ban period.
    timeKeeper->currentTime = 4.0;
    schedule.WakeUp();

    // Connect a fourth time, and expect this time to succeed.
    connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    ASSERT_FALSE(connection->dataReceivedDelegate == nullptr);
    EXPECT_FALSE(connection->broken);

    // Issue a good request and expect some response.
    request = (
        "GET / HTTP/1.1\r\n"
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
    ASSERT_FALSE(response == nullptr);
    EXPECT_FALSE(connection->broken);
}

TEST_F(ServerTests, BadRequestAfterProbationResetsBan) {
    // Set up server.
    auto transport = std::make_shared< MockTransport >();
    const auto timeKeeper = std::make_shared< MockTimeKeeper >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = timeKeeper;
    server.SetConfigurationItem("Port", "1234");
    server.SetConfigurationItem("InitialBanPeriod", "1.0");
    server.SetConfigurationItem("ProbationPeriod", "1.0");
    (void)server.Mobilize(deps);

    // Issue a couple bad requests, in order to double initial ban time.
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    std::string request(
        "Pog\0Champ This is a baaaaaaad request!\r\n\r\n",
        42
    );
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.end()
        )
    );
    connection = nullptr;
    timeKeeper->currentTime = 1.5;
    connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    EXPECT_FALSE(connection->dataReceivedDelegate == nullptr);
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.end()
        )
    );
    EXPECT_TRUE(connection->broken);
    EXPECT_TRUE(connection->brokenGracefully);
    connection = nullptr;

    // Advance time until the client is past the ban and probation periods.
    timeKeeper->currentTime = 5.0;

    // Issue a third bad request.
    connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    ASSERT_FALSE(connection->dataReceivedDelegate == nullptr);
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.end()
        )
    );
    EXPECT_TRUE(connection->broken);
    EXPECT_TRUE(connection->brokenGracefully);
    connection = nullptr;

    // Advance time one more initial ban period.
    timeKeeper->currentTime = 6.5;

    // Connect a fourth time, but expect to not be banned.
    connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    ASSERT_FALSE(connection->dataReceivedDelegate == nullptr);

    // Issue a good request and expect some response.
    request = (
        "GET / HTTP/1.1\r\n"
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
    ASSERT_FALSE(response == nullptr);
    EXPECT_FALSE(connection->broken);
}

TEST_F(ServerTests, TooManyRequestsResultsInBan) {
    // Set up server.
    auto transport = std::make_shared< MockTransport >();
    const auto timeKeeper = std::make_shared< MockTimeKeeper >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = timeKeeper;
    server.SetConfigurationItem("Port", "1234");
    server.SetConfigurationItem("InitialBanPeriod", "1.0");
    server.SetConfigurationItem("TooManyRequestsThreshold", "2.0");
    server.SetConfigurationItem("TooManyRequestsMeasurementPeriod", "1.0");
    (void)server.Mobilize(deps);

    // Issue two good requests, but too quickly,
    // and expect to be banned.
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    const std::string request = (
        "GET / HTTP/1.1\r\n"
        "Host: www.example.com\r\n"
        "\r\n"
    );
    ASSERT_FALSE(connection->dataReceivedDelegate == nullptr);
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.end()
        )
    );
    connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    ASSERT_FALSE(connection->dataReceivedDelegate == nullptr);
    diagnosticMessages.clear();
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
    ASSERT_FALSE(response == nullptr);
    EXPECT_EQ(429, response->statusCode);
    EXPECT_TRUE(connection->broken);
    EXPECT_TRUE(connection->brokenGracefully);
    ASSERT_EQ(
        (std::vector< std::string >{
            "Http::Server[1]: Request: GET '/' (0) from mock-client:5555: 429 (0)",
            "Http::Server[3]: Request: mock-client banned for 1 seconds (Bad HTTP: 429 Too Many Requests)",
            "Http::Server[2]: Connection to mock-client:5555 closed by server",
        }),
        diagnosticMessages
    );

    // Try to connect again, but expect to be banned.
    connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    EXPECT_TRUE(connection->dataReceivedDelegate == nullptr);

    // Advance time until the client is past the ban period.
    timeKeeper->currentTime = 1.5;

    // Connect a third time and expect to be no longer banned.
    connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    ASSERT_FALSE(connection->dataReceivedDelegate == nullptr);
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
    ASSERT_FALSE(response == nullptr);
    EXPECT_NE(429, response->statusCode);
    EXPECT_FALSE(connection->broken);
}

TEST_F(ServerTests, MultipleBadRequestsSentAtOnceCausesContinuedProcessingAfterClose) {
    // Set up server.
    auto transport = std::make_shared< MockTransport >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    server.SetConfigurationItem("BadRequestReportBytes", "4");
    (void)server.Mobilize(deps);

    // Start a new mock connection.
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);

    // Issue a bad request.
    diagnosticMessages.clear();
    const std::string request(
        "Pog1 This is a baaaaaaad request!\r\n\r\n"
        "Pog2 This is another baaaaaaad request!\r\n\r\n"
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
    ASSERT_EQ(
        (std::vector< std::string >{
            "Http::Server[3]: Request: Bad request from mock-client:5555: Pog1",
            "Http::Server[3]: Request: mock-client banned for 60 seconds (Bad HTTP: 400 Bad Request)",
            "Http::Server[2]: Connection to mock-client:5555 closed by server",
        }),
        diagnosticMessages
    );
}

TEST_F(ServerTests, GzippedResponse) {
    // Set up all the things.
    auto transport = std::make_shared< MockTransport >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    (void)server.Mobilize(deps);
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);

    // Register a delegate to handle the source, and then query the server
    // a second time.  Expect the resource request to be routed to the delegate
    // and handled correctly this time.
    std::vector< Uri::Uri > requestsReceived;
    const auto resourceDelegate = [&requestsReceived](
        const Http::Request& request,
        std::shared_ptr< Http::Connection > connection,
        const std::string& trailer
    ){
        Http::Response response;
        response.statusCode = 200;
        response.reasonPhrase = "OK";
        response.body = "Hello, World!";
        response.headers.SetHeader("Content-Encoding", "gzip");
        response.headers.SetHeader("Content-Length", "13");
        response.headers.SetHeader("Vary", "Accept-Encoding");
        requestsReceived.push_back(request.target);
        return response;
    };
    const auto unregistrationDelegate = server.RegisterResource({ "foo" }, resourceDelegate);

    // Send in request.
    const std::string request = (
        "GET /foo/bar HTTP/1.1\r\n"
        "Host: www.example.com\r\n"
        "Accept-Encoding: gzip\r\n"
        "\r\n"
    );
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.end()
        )
    );

    // Parse data received back as a response.
    Http::Client client;
    const auto response = client.ParseResponse(
        std::string(
            connection->dataReceived.begin(),
            connection->dataReceived.end()
        )
    );
    EXPECT_EQ(200, response->statusCode);
    ASSERT_EQ(1, requestsReceived.size());
    EXPECT_EQ("33", response->headers.GetHeaderValue("Content-Length"));
    EXPECT_EQ("gzip", response->headers.GetHeaderValue("Content-Encoding"));
    EXPECT_TRUE(response->headers.HasHeaderToken("Vary", "Accept-Encoding"));
    // NOTE: ignore 10th byte, because that contains an operating-system code,
    // which is obviously going to be different depending on what platform
    // we build and test this code.
    EXPECT_EQ(
        std::string(
            "\x1F\x8B\x08\x00\x00\x00\x00\x00\x00",
            9
        ),
        response->body.substr(0, 9)
    );
    EXPECT_EQ(
        std::string(
            "\xF3\x48\xCD\xC9\xC9\xD7"
            "\x51\x08\xCF\x2F\xCA\x49\x51\x04\x00\xD0\xC3\x4A\xEC\x0D\x00\x00"
            "\x00",
            23
        ),
        response->body.substr(10)
    );
    connection->dataReceived.clear();
}

TEST_F(ServerTests, DeflateResponse) {
    // Set up all the things.
    auto transport = std::make_shared< MockTransport >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    (void)server.Mobilize(deps);
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);

    // Register a delegate to handle the source, and then query the server
    // a second time.  Expect the resource request to be routed to the delegate
    // and handled correctly this time.
    std::vector< Uri::Uri > requestsReceived;
    const auto resourceDelegate = [&requestsReceived](
        const Http::Request& request,
        std::shared_ptr< Http::Connection > connection,
        const std::string& trailer
    ){
        Http::Response response;
        response.statusCode = 200;
        response.reasonPhrase = "OK";
        response.body = "Hello, World!";
        response.headers.SetHeader("Content-Encoding", "deflate");
        response.headers.SetHeader("Vary", "Accept-Encoding");
        requestsReceived.push_back(request.target);
        return response;
    };
    const auto unregistrationDelegate = server.RegisterResource({ "foo" }, resourceDelegate);

    // Send in request.
    const std::string request = (
        "GET /foo/bar HTTP/1.1\r\n"
        "Host: www.example.com\r\n"
        "Accept-Encoding: deflate\r\n"
        "\r\n"
    );
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.end()
        )
    );

    // Parse data received back as a response.
    Http::Client client;
    const auto response = client.ParseResponse(
        std::string(
            connection->dataReceived.begin(),
            connection->dataReceived.end()
        )
    );
    EXPECT_EQ(200, response->statusCode);
    ASSERT_EQ(1, requestsReceived.size());
    EXPECT_EQ("deflate", response->headers.GetHeaderValue("Content-Encoding"));
    EXPECT_TRUE(response->headers.HasHeaderToken("Vary", "Accept-Encoding"));
    EXPECT_EQ(
        std::string(
            "\x78\x9C\xF3\x48\xCD\xC9\xC9\xD7\x51\x08\xCF\x2F\xCA\x49\x51\x04"
            "\x00\x1F\x9E\x04\x6A",
            21
        ),
        response->body
    );
    connection->dataReceived.clear();
}

TEST_F(ServerTests, MaxMessageSizeCheckedForHeaders) {
    auto transport = std::make_shared< MockTransport >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    server.SetConfigurationItem("MaxMessageSize", "150");
    (void)server.Mobilize(deps);
    diagnosticMessages.clear();
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    const std::string smallRequest = (
        "GET /hello.txt HTTP/1.1\r\n"
        "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
        "Host: www.example.com\r\n"
        "Accept-Language: en, mi\r\n"
        "\r\n"
    );
    const std::string largeRequest = (
        "GET /hello.txt HTTP/1.1\r\n"
        "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
        "Host: www.example.com\r\n"
        "Accept-Language: en, mi\r\n"
        "X-PogChamp-Level: Over 9000\r\n"
        "\r\n"
    );
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            smallRequest.begin(),
            smallRequest.end()
        )
    );
    const std::string expectedResponseToSmallRequest = (
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "FeelsBadMan\r\n"
    );
    EXPECT_EQ(
        expectedResponseToSmallRequest,
        std::string(
            connection->dataReceived.begin(),
            connection->dataReceived.end()
        )
    );
    connection->dataReceived.clear();
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            largeRequest.begin(),
            largeRequest.end()
        )
    );
    const std::string expectedResponseToLargeRequest = (
        "HTTP/1.1 431 Request Header Fields Too Large\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "FeelsBadMan\r\n"
    );
    EXPECT_EQ(
        expectedResponseToLargeRequest,
        std::string(
            connection->dataReceived.begin(),
            connection->dataReceived.end()
        )
    );
    EXPECT_TRUE(connection->broken);
}

TEST_F(ServerTests, MaxMessageSizeCheckedForTotal) {
    auto transport = std::make_shared< MockTransport >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    server.SetConfigurationItem("MaxMessageSize", "125");
    (void)server.Mobilize(deps);
    diagnosticMessages.clear();
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    const std::string smallRequest = (
        "POST / HTTP/1.1\r\n"
        "Host: foo.com\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "say=Hi&to=Mom\r\n"
    );
    const std::string largeRequest = (
        "POST / HTTP/1.1\r\n"
        "Host: foo.com\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: 100\r\n"
        "\r\n"
        "say=Hi&to=Mom&listen_to=lecture&content=remember_to_brush_your_teeth_and_always_wear_clean_underwear\r\n"
    );
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            smallRequest.begin(),
            smallRequest.end()
        )
    );
    const std::string expectedResponse = (
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "FeelsBadMan\r\n"
    );
    EXPECT_EQ(
        expectedResponse,
        std::string(
            connection->dataReceived.begin(),
            connection->dataReceived.end()
        )
    );
    connection->dataReceived.clear();
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            largeRequest.begin(),
            largeRequest.end()
        )
    );
    const std::string expectedResponseToLargeRequest = (
        "HTTP/1.1 413 Payload Too Large\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "FeelsBadMan\r\n"
    );
    EXPECT_EQ(
        expectedResponseToLargeRequest,
        std::string(
            connection->dataReceived.begin(),
            connection->dataReceived.end()
        )
    );
    EXPECT_TRUE(connection->broken);
}

TEST_F(ServerTests, BlowingMaxMessageSizeInHeadersResultsInBan) {
    auto transport = std::make_shared< MockTransport >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    server.SetConfigurationItem("MaxMessageSize", "125");
    (void)server.Mobilize(deps);
    diagnosticMessages.clear();
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    const std::string largeRequest = (
        "GET /hello.txt HTTP/1.1\r\n"
        "User-Agent: curl/7.16.3 libcurl/7.16.3 OpenSSL/0.9.7l zlib/1.2.3\r\n"
        "Host: www.example.com\r\n"
        "Accept-Language: en, mi\r\n"
        "X-PogChamp-Level: Over 9000\r\n"
        "\r\n"
    );
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            largeRequest.begin(),
            largeRequest.end()
        )
    );
    connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    EXPECT_TRUE(connection->broken);
}

TEST_F(ServerTests, BlowingMaxMessageSizeInContentResultsInBan) {
    auto transport = std::make_shared< MockTransport >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    server.SetConfigurationItem("Port", "1234");
    server.SetConfigurationItem("MaxMessageSize", "125");
    (void)server.Mobilize(deps);
    diagnosticMessages.clear();
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    const std::string largeRequest = (
        "POST / HTTP/1.1\r\n"
        "Host: foo.com\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: 100\r\n"
        "\r\n"
        "say=Hi&to=Mom&listen_to=lecture&content=remember_to_brush_your_teeth_and_always_wear_clean_underwear\r\n"
    );
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            largeRequest.begin(),
            largeRequest.end()
        )
    );
    connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    EXPECT_TRUE(connection->broken);
}

TEST_F(ServerTests, ManuallyBanClient) {
    auto transport = std::make_shared< MockTransport >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    (void)server.Mobilize(deps);
    server.Ban("mock-client", "because I feel like it");
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    EXPECT_TRUE(connection->broken);
    EXPECT_FALSE(connection->brokenGracefully);
    EXPECT_EQ(
        std::set< std::string >({
            "mock-client",
        }),
        server.GetBans()
    );
}

TEST_F(ServerTests, BanDelegate) {
    // Arrange
    auto transport = std::make_shared< MockTransport >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    (void)server.Mobilize(deps);
    std::promise< void > wasBanned;
    std::string banPeerAddress;
    std::string banReason;
    auto unregister = server.RegisterBanDelegate(
        [&wasBanned, &banPeerAddress, &banReason](
            const std::string& peerAddress,
            const std::string& reason
        ) {
            banPeerAddress = peerAddress;
            banReason = reason;
            wasBanned.set_value();
        }
    );

    // Act
    server.Ban("mock-client", "because I feel like it");

    // Assert
    auto wasBannedFuture = wasBanned.get_future();
    ASSERT_EQ(
        std::future_status::ready,
        wasBannedFuture.wait_for(std::chrono::milliseconds(100))
    );
    EXPECT_EQ("mock-client", banPeerAddress);
    EXPECT_EQ("because I feel like it", banReason);
}

TEST_F(ServerTests, BanDelegateUnregistered) {
    // Arrange
    auto transport = std::make_shared< MockTransport >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    (void)server.Mobilize(deps);
    std::promise< void > wasBanned;
    std::string banPeerAddress;
    std::string banReason;
    auto unregister = server.RegisterBanDelegate(
        [&wasBanned, &banPeerAddress, &banReason](
            const std::string& peerAddress,
            const std::string& reason
        ) {
            banPeerAddress = peerAddress;
            banReason = reason;
            wasBanned.set_value();
        }
    );

    // Act
    unregister();
    server.Ban("mock-client", "because I feel like it");

    // Assert
    auto wasBannedFuture = wasBanned.get_future();
    ASSERT_NE(
        std::future_status::ready,
        wasBannedFuture.wait_for(std::chrono::milliseconds(100))
    );
}

TEST_F(ServerTests, ForceablyCloseConnectionThatLingersAfterGracefulClose) {
    const auto transport = std::make_shared< MockTransport >();
    const auto timeKeeper = std::make_shared< MockTimeKeeper >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = timeKeeper;
    server.SetConfigurationItem("Port", "1234");
    server.SetConfigurationItem("InactivityTimeout", "1.0");
    server.SetConfigurationItem("GracefulCloseTimeout", "1.0");
    (void)server.Mobilize(deps);
    auto& scheduler = server.GetScheduler();
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    const std::string request = (
        "GET /foo/bar HTTP/1.1\r\n"
        "Host: www.example.com\r\n"
    );
    connection->dataReceivedDelegate(
        std::vector< uint8_t >(
            request.begin(),
            request.end()
        )
    );
    timeKeeper->currentTime = 1.001;
    scheduler.WakeUp();
    ASSERT_TRUE(connection->AwaitBroken());
    EXPECT_TRUE(connection->brokenGracefully);
    connection->broken = false;
    timeKeeper->currentTime = 2.002;
    scheduler.WakeUp();
    ASSERT_TRUE(connection->AwaitBroken());
    EXPECT_FALSE(connection->brokenGracefully);
}

TEST_F(ServerTests, ConnectionRateLimit) {
    // Arrange
    auto transport = std::make_shared< MockTransport >();
    const auto timeKeeper = std::make_shared< MockTimeKeeper >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = timeKeeper;
    server.SetConfigurationItem("TooManyConnectsThreshold", "1.0");
    server.SetConfigurationItem("TooManyConnectsMeasurementPeriod", "1.0");
    (void)server.Mobilize(deps);

    // Act
    const auto connection1 = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection1);
    timeKeeper->currentTime = 0.9;
    const auto connection2 = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection2);
    timeKeeper->currentTime = 1.1;
    const auto connection3 = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection3);
    timeKeeper->currentTime = 2.2;
    const auto connection4 = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection4);
    timeKeeper->currentTime = 3.1;
    const auto connection5 = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection5);

    // Assert
    EXPECT_FALSE(connection1->broken);
    EXPECT_TRUE(connection2->broken);
    EXPECT_FALSE(connection3->broken);
    EXPECT_FALSE(connection4->broken);
    EXPECT_TRUE(connection5->broken);
}

TEST_F(ServerTests, WhitelistedClientsAllowedThroughNotBlacklisted) {
    // Arrange
    auto transport = std::make_shared< MockTransport >();
    const auto timeKeeper = std::make_shared< MockTimeKeeper >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = timeKeeper;
    server.SetConfigurationItem("TooManyConnectsThreshold", "1.0");
    server.SetConfigurationItem("TooManyConnectsMeasurementPeriod", "1.0");
    EXPECT_EQ(
        std::set< std::string >({
        }),
        server.GetWhitelist()
    );
    server.WhitelistAdd("admin");
    EXPECT_EQ(
        std::set< std::string >({
            "admin"
        }),
        server.GetWhitelist()
    );
    (void)server.Mobilize(deps);

    // Act
    const auto connection1 = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection1);
    timeKeeper->currentTime = 0.9;
    const auto connection2 = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection2);
    const auto connection3 = std::make_shared< MockConnection >();
    connection3->peerAddress = "admin";
    transport->connectionDelegate(connection3);
    server.WhitelistRemove("admin");
    EXPECT_EQ(
        std::set< std::string >({
        }),
        server.GetWhitelist()
    );
    const auto connection4 = std::make_shared< MockConnection >();
    connection4->peerAddress = "admin";
    transport->connectionDelegate(connection4);

    // Assert
    EXPECT_FALSE(connection1->broken);
    EXPECT_TRUE(connection2->broken);
    EXPECT_FALSE(connection3->broken);
    EXPECT_TRUE(connection4->broken);
}

TEST_F(ServerTests, Unban) {
    // Arrange
    auto transport = std::make_shared< MockTransport >();
    Http::Server::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    (void)server.Mobilize(deps);
    server.Ban("mock-client", "because I feel like it");

    // Act
    server.Unban("mock-client");

    // Assert
    auto connection = std::make_shared< MockConnection >();
    transport->connectionDelegate(connection);
    EXPECT_FALSE(connection->broken);
}

TEST_F(ServerTests, BanNotListedIfBanTimedOut) {
    // Arrange
    auto transport = std::make_shared< MockTransport >();
    Http::Server::MobilizationDependencies deps;
    const auto timeKeeper = std::make_shared< MockTimeKeeper >();
    deps.transport = transport;
    deps.timeKeeper = timeKeeper;
    server.SetConfigurationItem("InitialBanPeriod", "1.0");
    (void)server.Mobilize(deps);
    server.Ban("mock-client", "because I feel like it");

    // Act
    timeKeeper->currentTime += 1.1;

    // Assert
    EXPECT_EQ(
        std::set< std::string >({
        }),
        server.GetBans()
    );
}
