/**
 * @file ClientTests.cpp
 *
 * This module contains the unit tests of the
 * Http::Client class.
 *
 * © 2018 by Richard Walters
 */

#include <condition_variable>
#include <gtest/gtest.h>
#include <Http/Client.hpp>
#include <Http/Request.hpp>
#include <Http/Server.hpp>
#include <inttypes.h>
#include <memory>
#include <mutex>
#include <SystemAbstractions/DiagnosticsSender.hpp>
#include <SystemAbstractions/StringExtensions.hpp>

namespace {

    /**
     * This is a fake server connection which is used to test the client.
     */
    struct MockConnection
        : public Http::Connection
    {
        // Properties

        /**
         * This is used to synchronize access to the wait condition.
         */
        std::recursive_mutex mutex;

        /**
         * This is used to wait for, or signal, a condition
         * upon which that the tests might be waiting.
         */
        std::condition_variable_any waitCondition;

        /**
         * This is the host name or IP address of the mock server.
         */
        std::string hostNameOrIpAddress;

        /**
         * This is the port number of the mock server.
         */
        uint16_t port = 80;

        /**
         * This buffer is used to reassemble fragmented HTTP requests
         * received from the client.
         */
        std::string reassemblyBuffer;

        /**
         * This is only used to parse HTTP requests from the unit under test.
         */
        Http::Server server;

        /**
         * This is the collection of requests issued by the unit under test.
         */
        std::vector< Http::Request > requests;

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
         * This flag is set if the remote peer breaks the connection.
         */
        bool broken = false;

        /**
         * This flag indicates whether or not the remote peer broke
         * the connection gracefully.
         */
        bool brokenGracefully = false;

        // Lifecycle management
        ~MockConnection() noexcept = default;
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
         * This method waits for the unit under test to issue the given
         * number of requests.
         *
         * @param[in] numRequests
         *     This is the number of requests to await.
         *
         * @return
         *     An indication of whether or not the given number of requests
         *     were received from the unit under test before a reasonable
         *     timeout period has elapsed is returned.
         */
        bool AwaitRequests(size_t numRequests) {
            std::unique_lock< decltype(mutex) > lock(mutex);
            return waitCondition.wait_for(
                lock,
                std::chrono::milliseconds(100),
                [this, numRequests]{ return requests.size() >= numRequests; }
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
            return hostNameOrIpAddress;
        }

        virtual std::string GetPeerId() override {
            return SystemAbstractions::sprintf("%s:%" PRIu16, hostNameOrIpAddress, port);
        }

        virtual void SetDataReceivedDelegate(DataReceivedDelegate newDataReceivedDelegate) override {
            dataReceivedDelegate = newDataReceivedDelegate;
        }

        virtual void SetBrokenDelegate(BrokenDelegate newBrokenDelegate) override {
            brokenDelegate = newBrokenDelegate;
        }

        virtual void SendData(const std::vector< uint8_t >& data) override {
            std::lock_guard< decltype(mutex) > lock(mutex);
            reassemblyBuffer += std::string(data.begin(), data.end());
            size_t messageEnd;
            const auto request = server.ParseRequest(reassemblyBuffer, messageEnd);
            if (request != nullptr) {
                if (messageEnd == reassemblyBuffer.size()) {
                    reassemblyBuffer.clear();
                } else {
                    reassemblyBuffer.erase(
                        reassemblyBuffer.begin(),
                        reassemblyBuffer.begin() + messageEnd
                    );
                }
                requests.push_back(*request);
                waitCondition.notify_all();
            }
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
        : public Http::ClientTransport
    {
        // Properties

        /**
         * This is used to synchronize access to the wait condition.
         */
        std::recursive_mutex mutex;

        /**
         * This is used to wait for, or signal, a condition
         * upon which that the tests might be waiting.
         */
        std::condition_variable_any waitCondition;

        /**
         * This is the number of connections the client is allowed
         * to establish.
         */
        size_t connectionsAllowed = 1;

        /**
         * These are the connection objects created by the client
         * to talk to servers.
         */
        std::vector< std::shared_ptr< MockConnection > > connections;

        // Methods

        /**
         * This method waits for the client to establish the given
         * number of connections to servers.
         *
         * @param[in] numConnections
         *     This is the number of connections for which to await.
         *
         * @return
         *     An indication of whether or not the given number of
         *     connections were established by the client before a reasonable
         *     timeout period has elapsed is returned.
         */
        bool AwaitConnections(size_t numConnections) {
            std::unique_lock< decltype(mutex) > lock(mutex);
            return waitCondition.wait_for(
                lock,
                std::chrono::milliseconds(100),
                [this, numConnections]{ return connections.size() >= numConnections; }
            );
        }

        // Http::ClientTransport

        virtual std::shared_ptr< Http::Connection > Connect(
            const std::string& hostNameOrAddress,
            uint16_t port,
            Http::Connection::DataReceivedDelegate dataReceivedDelegate,
            Http::Connection::BrokenDelegate brokenDelegate
        ) override {
            if (connectionsAllowed > 0) {
                --connectionsAllowed;
                const auto connection = std::make_shared< MockConnection >();
                connection->hostNameOrIpAddress = hostNameOrAddress;
                connection->port = port;
                connection->SetDataReceivedDelegate(dataReceivedDelegate);
                connection->SetBrokenDelegate(brokenDelegate);
                {
                    std::unique_lock< decltype(mutex) > lock(mutex);
                    connections.push_back(connection);
                    waitCondition.notify_all();
                }
                return connection;
            } else {
                return nullptr;
            }
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
struct ClientTests
    : public ::testing::Test
{
    // Properties

    /**
     * This is the unit under test.
     */
    Http::Client client;

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
        diagnosticsUnsubscribeDelegate = client.SubscribeToDiagnostics(
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
        client.Demobilize();
        diagnosticsUnsubscribeDelegate();
    }
};

TEST_F(ClientTests, ParseGetResponse) {
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
    ASSERT_EQ(Http::Response::State::Complete, response->state);
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

TEST_F(ClientTests, ParseIncompleteBodyResponse) {
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

TEST_F(ClientTests, ParseIncompleteHeadersBetweenLinesResponse) {
    const auto response = client.ParseResponse(
        "HTTP/1.1 200 OK\r\n"
        "Date: Mon, 27 Jul 2009 12:28:53 GMT\r\n"
        "Server: Apache\r\n"
        "Last-Modified: Wed, 22 Jul 2009 19:15:56 GMT\r\n"
        "ETag: \"34aa387-d-1568eb00\"\r\n"
    );
    ASSERT_TRUE(response == nullptr);
}

TEST_F(ClientTests, ParseIncompleteHeadersMidLineResponse) {
    const auto response = client.ParseResponse(
        "HTTP/1.1 200 OK\r\n"
        "Date: Mon, 27 Jul 2009 12:28:53 GMT\r\n"
        "Server: Apache\r\n"
        "Last-Modified: Wed, 22 Ju"
    );
    ASSERT_TRUE(response == nullptr);
}

TEST_F(ClientTests, ParseIncompleteStatusLine) {
    const auto response = client.ParseResponse(
        "HTTP/1.1 200 OK\r"
    );
    ASSERT_TRUE(response == nullptr);
}

TEST_F(ClientTests, ParseNoHeadersResponse) {
    const auto response = client.ParseResponse(
        "HTTP/1.1 200 OK\r\n"
    );
    ASSERT_TRUE(response == nullptr);
}

TEST_F(ClientTests, ParseInvalidResponseNoProtocol) {
    size_t messageEnd;
    const std::string rawResponse = (
        " 200 OK\r\n"
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
    const auto response = client.ParseResponse(rawResponse, messageEnd);
    ASSERT_FALSE(response == nullptr);
    ASSERT_EQ(Http::Response::State::Complete, response->state);
    ASSERT_FALSE(response->valid);
}

TEST_F(ClientTests, ParseInvalidResponseNoStatusCode) {
    size_t messageEnd;
    const std::string rawResponse = (
        "HTTP/1.1  OK\r\n"
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
    const auto response = client.ParseResponse(rawResponse, messageEnd);
    ASSERT_FALSE(response == nullptr);
    ASSERT_EQ(Http::Response::State::Complete, response->state);
    ASSERT_FALSE(response->valid);
}

TEST_F(ClientTests, ParseInvalidResponseNoReasonPhrase) {
    size_t messageEnd;
    const std::string rawResponse = (
        "HTTP/1.1 200\r\n"
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
    const auto response = client.ParseResponse(rawResponse, messageEnd);
    ASSERT_FALSE(response == nullptr);
    ASSERT_EQ(Http::Response::State::Complete, response->state);
    ASSERT_FALSE(response->valid);
}

TEST_F(ClientTests, ParseInvalidDamagedHeader) {
    size_t messageEnd;
    const std::string rawResponse = (
        "HTTP/1.1 200\r\n"
        "Date: Mon, 27 Jul 2009 12:28:53 GMT\r\n"
        "Server: Apache\r\n"
        "Last-Modified Wed, 22 Jul 2009 19:15:56 GMT\r\n"
        "ETag: \"34aa387-d-1568eb00\"\r\n"
        "Accept-Ranges: bytes\r\n"
        "Content-Length: 51\r\n"
        "Vary: Accept-Encoding\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "Hello World! My payload includes a trailing CRLF.\r\n"
    );
    const auto response = client.ParseResponse(rawResponse, messageEnd);
    ASSERT_FALSE(response == nullptr);
    ASSERT_EQ(Http::Response::State::Complete, response->state);
    ASSERT_FALSE(response->valid);
    ASSERT_EQ(rawResponse.length(), messageEnd);
}

TEST_F(ClientTests, ResponseWithNoContentLengthOrChunkedTransferEncodingHasNoBody) {
    size_t messageEnd;
    const std::string rawResponse = (
        "HTTP/1.1 200 OK\r\n"
        "Date: Mon, 27 Jul 2009 12:28:53 GMT\r\n"
        "Server: Apache\r\n"
        "Last-Modified: Wed, 22 Jul 2009 19:15:56 GMT\r\n"
        "ETag: \"34aa387-d-1568eb00\"\r\n"
        "Accept-Ranges: bytes\r\n"
        "Vary: Accept-Encoding\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
    );
    const std::string trailer = "Hello World! My payload includes a trailing CRLF.\r\n";
    const auto response = client.ParseResponse(rawResponse + trailer, messageEnd);
    ASSERT_FALSE(response == nullptr);
    ASSERT_EQ("", response->body);
    ASSERT_EQ(rawResponse.size(), messageEnd);
}

TEST_F(ClientTests, SimpleGetRequestOnePieceResponse) {
    // Set up the client.
    const auto transport = std::make_shared< MockTransport >();
    Http::Client::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    client.Mobilize(deps);

    // Have the client make a simple request.
    Http::Request outgoingRequest;
    outgoingRequest.method = "GET";
    outgoingRequest.target.ParseFromString("http://www.example.com:1234/foo");
    const auto transaction = client.Request(outgoingRequest);
    ASSERT_FALSE(transaction == nullptr);
    ASSERT_TRUE(transport->AwaitConnections(1));
    const auto& connection = transport->connections[0];
    EXPECT_EQ("www.example.com", connection->hostNameOrIpAddress);
    EXPECT_EQ(1234, connection->port);
    ASSERT_TRUE(connection->AwaitRequests(1));
    const auto& incomingRequest = connection->requests[0];
    EXPECT_EQ("GET", incomingRequest.method);
    EXPECT_EQ((std::vector< std::string >{"", "foo"}), incomingRequest.target.GetPath());
    EXPECT_EQ("www.example.com", incomingRequest.headers.GetHeaderValue("Host"));

    // Provide a response back to the client, in one piece.
    Http::Response response;
    response.statusCode = 200;
    response.reasonPhrase = "OK";
    response.headers.SetHeader("Foo", "Bar");
    response.headers.SetHeader("Content-Type", "text/plain");
    response.headers.SetHeader("Content-Length", "8");
    response.body = "PogChamp";
    const auto& responseEncoding = response.Generate();
    connection->dataReceivedDelegate({responseEncoding.begin(), responseEncoding.end()});

    // Wait for client transaction to complete.
    ASSERT_TRUE(transaction->AwaitCompletion(std::chrono::milliseconds(100)));
    EXPECT_EQ(200, transaction->response.statusCode);
    EXPECT_EQ("OK", transaction->response.reasonPhrase);
    EXPECT_EQ("Bar", transaction->response.headers.GetHeaderValue("Foo"));
    EXPECT_EQ("text/plain", transaction->response.headers.GetHeaderValue("Content-Type"));
    EXPECT_EQ("8", transaction->response.headers.GetHeaderValue("Content-Length"));
    EXPECT_EQ("PogChamp", transaction->response.body);
}

TEST_F(ClientTests, SimpleGetRequestFragmentedResponse) {
    // Set up the client.
    const auto transport = std::make_shared< MockTransport >();
    Http::Client::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    client.Mobilize(deps);

    // Have the client make a simple request.
    Http::Request outgoingRequest;
    outgoingRequest.method = "GET";
    outgoingRequest.target.ParseFromString("http://www.example.com:1234/foo");
    const auto transaction = client.Request(outgoingRequest);
    ASSERT_FALSE(transaction == nullptr);
    ASSERT_TRUE(transport->AwaitConnections(1));
    const auto& connection = transport->connections[0];
    EXPECT_EQ("www.example.com", connection->hostNameOrIpAddress);
    EXPECT_EQ(1234, connection->port);
    ASSERT_TRUE(connection->AwaitRequests(1));
    const auto& incomingRequest = connection->requests[0];
    EXPECT_EQ("GET", incomingRequest.method);
    EXPECT_EQ((std::vector< std::string >{"", "foo"}), incomingRequest.target.GetPath());
    EXPECT_EQ("www.example.com", incomingRequest.headers.GetHeaderValue("Host"));

    // Provide a response back to the client, in fragments.
    Http::Response response;
    response.statusCode = 200;
    response.reasonPhrase = "OK";
    response.headers.SetHeader("Foo", "Bar");
    response.headers.SetHeader("Content-Type", "text/plain");
    response.headers.SetHeader("Content-Length", "8");
    response.body = "PogChamp";
    const auto& responseEncoding = response.Generate();
    for (size_t i = 0; i < responseEncoding.size(); ++i) {
        connection->dataReceivedDelegate({(uint8_t)responseEncoding[i]});
        if (i == responseEncoding.size() - 2) {
            ASSERT_FALSE(transaction->AwaitCompletion(std::chrono::milliseconds(100)));
        }
    }

    // Wait for client transaction to complete.
    ASSERT_TRUE(transaction->AwaitCompletion(std::chrono::milliseconds(100)));
    EXPECT_EQ(200, transaction->response.statusCode);
    EXPECT_EQ("OK", transaction->response.reasonPhrase);
    EXPECT_EQ("Bar", transaction->response.headers.GetHeaderValue("Foo"));
    EXPECT_EQ("text/plain", transaction->response.headers.GetHeaderValue("Content-Type"));
    EXPECT_EQ("8", transaction->response.headers.GetHeaderValue("Content-Length"));
    EXPECT_EQ("PogChamp", transaction->response.body);
}

TEST_F(ClientTests, ConnectionReleasedAfterTransactionCompleted) {
    // Set up the client.
    const auto transport = std::make_shared< MockTransport >();
    Http::Client::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    client.Mobilize(deps);

    // Have the client make a simple request.
    Http::Request outgoingRequest;
    outgoingRequest.method = "GET";
    outgoingRequest.target.ParseFromString("http://www.example.com:1234/foo");
    const auto transaction = client.Request(outgoingRequest, false);
    auto connection = transport->connections[0];
    const auto& incomingRequest = connection->requests[0];

    // Provide a response back to the client, in one piece.
    Http::Response response;
    response.statusCode = 200;
    response.reasonPhrase = "OK";
    response.headers.SetHeader("Foo", "Bar");
    response.headers.SetHeader("Content-Type", "text/plain");
    response.headers.SetHeader("Content-Length", "8");
    response.body = "PogChamp";
    const auto& responseEncoding = response.Generate();
    connection->dataReceivedDelegate({responseEncoding.begin(), responseEncoding.end()});

    // Wait for client transaction to complete.
    ASSERT_TRUE(transaction->AwaitCompletion(std::chrono::milliseconds(100)));

    // Verify the connection was released.
    std::weak_ptr< MockConnection > connectionWeak(connection);
    connection = nullptr;
    transport->connections.clear();
    ASSERT_TRUE(connectionWeak.lock() == nullptr);
}

TEST_F(ClientTests, NonPersistentConnectionClosedProperly) {
    // Set up the client.
    const auto transport = std::make_shared< MockTransport >();
    Http::Client::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    client.Mobilize(deps);

    // Have the client make a simple request.
    Http::Request outgoingRequest;
    outgoingRequest.method = "GET";
    outgoingRequest.target.ParseFromString("http://www.example.com:1234/foo");
    const auto transaction = client.Request(outgoingRequest, false);
    auto connection = transport->connections[0];
    const auto& incomingRequest = connection->requests[0];
    EXPECT_TRUE(incomingRequest.headers.HasHeaderToken("Connection", "Close"));
    EXPECT_TRUE(connection->broken);
    connection->broken = false;
    EXPECT_TRUE(connection->brokenGracefully);

    // Provide a response back to the client, in one piece.
    Http::Response response;
    response.statusCode = 200;
    response.reasonPhrase = "OK";
    response.headers.SetHeader("Foo", "Bar");
    response.headers.SetHeader("Connection", "Close");
    response.headers.SetHeader("Content-Type", "text/plain");
    response.headers.SetHeader("Content-Length", "8");
    response.body = "PogChamp";
    const auto& responseEncoding = response.Generate();
    connection->dataReceivedDelegate({responseEncoding.begin(), responseEncoding.end()});

    // Wait for client transaction to complete.
    ASSERT_TRUE(transaction->AwaitCompletion(std::chrono::milliseconds(100)));

    // Wait for the client to close their end of the connection.
    ASSERT_TRUE(connection->AwaitBroken());
    ASSERT_FALSE(connection->brokenGracefully);
}
