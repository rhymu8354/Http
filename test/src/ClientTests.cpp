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
#include <stdlib.h>
#include <SystemAbstractions/DiagnosticsSender.hpp>
#include <SystemAbstractions/StringExtensions.hpp>

namespace {

    /**
     * This flag is used to cause a crash in MockConnection's destructor
     * if it's called while the flag is set to true.
     */
    bool crashOnConnectionDestruction = false;

    /**
     * This flag is set whenever a MockConnection is destroyed.
     */
    bool connectionDestroyed = false;

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

        /**
         * This flag indicates whether or not the mock connection
         * should deliver some data right before breaking its end
         * of the connection, when receiving an abrupt break.
         */
        bool sendDataJustBeforeBreak = false;

        // Lifecycle management
        ~MockConnection() noexcept {
            if (crashOnConnectionDestruction) {
                abort();
            }
            connectionDestroyed = true;
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
            return SystemAbstractions::sprintf("%s:%" PRIu16, hostNameOrIpAddress.c_str(), port);
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
            if (!clean) {
                if (sendDataJustBeforeBreak) {
                    dataReceivedDelegate({42});
                }
                brokenDelegate(false);
            }
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

TEST_F(ClientTests, ParseGetResponseWithBodyAndContentLength) {
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
    ASSERT_TRUE(response->headers.HasHeader("Content-Length"));
    ASSERT_EQ("51", response->headers.GetHeaderValue("Content-Length"));
    ASSERT_EQ("Hello World! My payload includes a trailing CRLF.\r\n", response->body);
}

TEST_F(ClientTests, ParseGetResponseWithChunkedBodyNoOtherTransferCoding) {
    const auto response = client.ParseResponse(
        "HTTP/1.1 200 OK\r\n"
        "Date: Mon, 27 Jul 2009 12:28:53 GMT\r\n"
        "Server: Apache\r\n"
        "Last-Modified: Wed, 22 Jul 2009 19:15:56 GMT\r\n"
        "ETag: \"34aa387-d-1568eb00\"\r\n"
        "Accept-Ranges: bytes\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Vary: Accept-Encoding\r\n"
        "Content-Type: text/plain\r\n"
        "Trailer: X-Foo\r\n"
        "\r\n"
        "C\r\n"
        "Hello World!\r\n"
        "16\r\n"
        " My payload includes a\r\n"
        "11\r\n"
        " trailing CRLF.\r\n\r\n"
        "0\r\n"
        "X-Foo: Bar\r\n"
        "\r\n"
    );
    ASSERT_FALSE(response == nullptr);
    ASSERT_EQ(Http::Response::State::Complete, response->state);
    EXPECT_EQ(200, response->statusCode);
    EXPECT_EQ("OK", response->reasonPhrase);
    EXPECT_EQ("Mon, 27 Jul 2009 12:28:53 GMT", response->headers.GetHeaderValue("Date"));
    EXPECT_EQ("bytes", response->headers.GetHeaderValue("Accept-Ranges"));
    EXPECT_EQ("text/plain", response->headers.GetHeaderValue("Content-Type"));
    EXPECT_EQ("Bar", response->headers.GetHeaderValue("X-Foo"));
    EXPECT_EQ("51", response->headers.GetHeaderValue("Content-Length"));
    EXPECT_FALSE(response->headers.HasHeaderToken("Transfer-Encoding", "chunked"));
    EXPECT_FALSE(response->headers.HasHeader("Trailer"));
    EXPECT_FALSE(response->headers.HasHeader("Transfer-Encoding"));
    EXPECT_EQ("Hello World! My payload includes a trailing CRLF.\r\n", response->body);
}

TEST_F(ClientTests, ParseGetResponseWithChunkedBodyWithOtherTransferCoding) {
    const auto response = client.ParseResponse(
        "HTTP/1.1 200 OK\r\n"
        "Date: Mon, 27 Jul 2009 12:28:53 GMT\r\n"
        "Transfer-Encoding: foobar, chunked\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "0\r\n"
        "\r\n"
    );
    ASSERT_FALSE(response == nullptr);
    ASSERT_EQ(Http::Response::State::Complete, response->state);
    EXPECT_EQ("foobar", response->headers.GetHeaderValue("Transfer-Encoding"));
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
    outgoingRequest.target.ParseFromString("http://PePe@www.example.com:1234/foo?abc#def");
    const auto transaction = client.Request(outgoingRequest);
    ASSERT_FALSE(transaction == nullptr);
    EXPECT_EQ(Http::Client::Transaction::State::InProgress, transaction->state);
    ASSERT_TRUE(transport->AwaitConnections(1));
    const auto& connection = transport->connections[0];
    EXPECT_EQ("www.example.com", connection->hostNameOrIpAddress);
    EXPECT_EQ(1234, connection->port);
    ASSERT_TRUE(connection->AwaitRequests(1));
    const auto& incomingRequest = connection->requests[0];
    EXPECT_EQ("GET", incomingRequest.method);
    EXPECT_TRUE(incomingRequest.target.IsRelativeReference());
    EXPECT_TRUE(incomingRequest.target.GetHost().empty());
    EXPECT_FALSE(incomingRequest.target.HasPort());
    EXPECT_TRUE(incomingRequest.target.GetUserInfo().empty());
    EXPECT_EQ((std::vector< std::string >{"", "foo"}), incomingRequest.target.GetPath());
    EXPECT_EQ("abc", incomingRequest.target.GetQuery());
    EXPECT_EQ("def", incomingRequest.target.GetFragment());
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
    EXPECT_EQ(Http::Client::Transaction::State::Completed, transaction->state);
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
    EXPECT_FALSE(incomingRequest.target.HasQuery());;
    EXPECT_FALSE(incomingRequest.target.HasFragment());

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

TEST_F(ClientTests, NonPersistentConnectionReleasedAfterTransactionCompleted) {
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
    auto transaction = client.Request(outgoingRequest, false);
    auto connection = transport->connections[0];

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
    EXPECT_EQ(Http::Client::Transaction::State::Completed, transaction->state);

    // Release the transaction (otherwise, it holds onto the connection).
    transaction = nullptr;

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
    auto transaction = client.Request(outgoingRequest, false);
    auto connection = transport->connections[0];
    connection->sendDataJustBeforeBreak = true;
    const auto& incomingRequest = connection->requests[0];
    EXPECT_TRUE(incomingRequest.headers.HasHeaderToken("Connection", "Close"));
    EXPECT_FALSE(connection->broken);

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
    EXPECT_TRUE(connection->broken);
    EXPECT_FALSE(connection->brokenGracefully);

    // Release all strong connection references, and verify the connection is not yet
    // destroyed (through a global flag which causes the connection destructor to
    // crash the test).
    crashOnConnectionDestruction = true;
    connection = nullptr;
    transport->connections.clear();

    // Release the transaction, and verify the connection is finally destroyed.
    crashOnConnectionDestruction = false;
    connectionDestroyed = false;
    transaction = nullptr;
    EXPECT_TRUE(connectionDestroyed);
}

TEST_F(ClientTests, TwoRequestsSameServerReusesPersistentConnection) {
    // Set up the client.
    const auto transport = std::make_shared< MockTransport >();
    transport->connectionsAllowed = 2;
    Http::Client::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    client.Mobilize(deps);

    // Have the client make a simple request, using a persistent connection.
    Http::Request outgoingRequest;
    outgoingRequest.method = "GET";
    outgoingRequest.target.ParseFromString("http://www.example.com:1234/foo");
    auto transaction = client.Request(outgoingRequest, true);
    const auto& connection = transport->connections[0];
    auto incomingRequest = connection->requests[0];
    EXPECT_FALSE(connection->broken);
    EXPECT_FALSE(incomingRequest.headers.HasHeaderToken("Connection", "Close"));

    // Provide a response back to the client, in one piece.
    Http::Response response;
    response.statusCode = 200;
    response.reasonPhrase = "OK";
    response.headers.SetHeader("Foo", "Bar");
    response.headers.SetHeader("Content-Type", "text/plain");
    response.headers.SetHeader("Content-Length", "8");
    response.body = "PogChamp";
    auto responseEncoding = response.Generate();
    connection->dataReceivedDelegate({responseEncoding.begin(), responseEncoding.end()});

    // Wait for client transaction to complete.
    ASSERT_TRUE(transaction->AwaitCompletion(std::chrono::milliseconds(100)));

    // Have the client make another simple request to the same server.
    // Expect the same connection to be reused.
    outgoingRequest = Http::Request();
    outgoingRequest.method = "GET";
    outgoingRequest.target.ParseFromString("http://www.example.com:1234/bar");
    transaction = client.Request(outgoingRequest);
    ASSERT_FALSE(transaction == nullptr);
    ASSERT_FALSE(transport->AwaitConnections(2));
    ASSERT_TRUE(connection->AwaitRequests(2));
    incomingRequest = connection->requests[1];
    EXPECT_EQ("GET", incomingRequest.method);
    EXPECT_EQ((std::vector< std::string >{"", "bar"}), incomingRequest.target.GetPath());
    EXPECT_EQ("www.example.com", incomingRequest.headers.GetHeaderValue("Host"));

    // Provide a response back to the client, in one piece.
    response = Http::Response();
    response.statusCode = 200;
    response.reasonPhrase = "OK";
    response.headers.SetHeader("Foo", "Hello");
    response.headers.SetHeader("Content-Type", "text/plain");
    response.headers.SetHeader("Content-Length", "7");
    response.body = "Poggers";
    responseEncoding = response.Generate();
    connection->dataReceivedDelegate({responseEncoding.begin(), responseEncoding.end()});

    // Wait for client transaction to complete.
    ASSERT_TRUE(transaction->AwaitCompletion(std::chrono::milliseconds(100)));
    EXPECT_EQ(200, transaction->response.statusCode);
    EXPECT_EQ("OK", transaction->response.reasonPhrase);
    EXPECT_EQ("Hello", transaction->response.headers.GetHeaderValue("Foo"));
    EXPECT_EQ("text/plain", transaction->response.headers.GetHeaderValue("Content-Type"));
    EXPECT_EQ("7", transaction->response.headers.GetHeaderValue("Content-Length"));
    EXPECT_EQ("Poggers", transaction->response.body);
}

TEST_F(ClientTests, SecondRequestNonPersistentWithPersistentConnectionClosesConnection) {
    // Set up the client.
    const auto transport = std::make_shared< MockTransport >();
    transport->connectionsAllowed = 2;
    Http::Client::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    client.Mobilize(deps);

    // Have the client make a simple request, using a persistent connection.
    Http::Request outgoingRequest;
    outgoingRequest.method = "GET";
    outgoingRequest.target.ParseFromString("http://www.example.com:1234/foo");
    auto transaction = client.Request(outgoingRequest, true);
    auto connection = transport->connections[0];

    // Provide a response back to the client, in one piece.
    Http::Response response;
    response.statusCode = 200;
    response.reasonPhrase = "OK";
    response.headers.SetHeader("Foo", "Bar");
    response.headers.SetHeader("Content-Type", "text/plain");
    response.headers.SetHeader("Content-Length", "8");
    response.body = "PogChamp";
    auto responseEncoding = response.Generate();
    connection->dataReceivedDelegate({responseEncoding.begin(), responseEncoding.end()});

    // Wait for client transaction to complete.
    ASSERT_TRUE(transaction->AwaitCompletion(std::chrono::milliseconds(100)));

    // Have the client make another simple request to the same server,
    // this time with a non-persistent connection.
    outgoingRequest = Http::Request();
    outgoingRequest.method = "GET";
    outgoingRequest.target.ParseFromString("http://www.example.com:1234/bar");
    transaction = client.Request(outgoingRequest, false);
    ASSERT_TRUE(connection->AwaitRequests(2));
    auto incomingRequest = connection->requests[1];
    EXPECT_TRUE(incomingRequest.headers.HasHeaderToken("Connection", "Close"));

    // Provide a response back to the client, in one piece.
    response = Http::Response();
    response.statusCode = 200;
    response.reasonPhrase = "OK";
    response.headers.SetHeader("Foo", "Hello");
    response.headers.SetHeader("Connection", "Close");
    response.headers.SetHeader("Content-Type", "text/plain");
    response.headers.SetHeader("Content-Length", "7");
    response.body = "Poggers";
    responseEncoding = response.Generate();
    connection->dataReceivedDelegate({responseEncoding.begin(), responseEncoding.end()});

    // Wait for client transaction to complete.
    ASSERT_TRUE(transaction->AwaitCompletion(std::chrono::milliseconds(100)));
    EXPECT_TRUE(connection->broken);
    EXPECT_FALSE(connection->brokenGracefully);
    connection->Break(false);

    // Have the client make a third simple request to the same server.
    // Expect a new connection to be made this time.
    outgoingRequest = Http::Request();
    outgoingRequest.method = "GET";
    outgoingRequest.target.ParseFromString("http://www.example.com:1234/spam");
    transaction = client.Request(outgoingRequest);
    ASSERT_FALSE(transaction == nullptr);
    ASSERT_TRUE(transport->AwaitConnections(2));
    connection = transport->connections[1];
    ASSERT_TRUE(connection->AwaitRequests(1));
    incomingRequest = connection->requests[0];
    EXPECT_EQ("GET", incomingRequest.method);
    EXPECT_EQ((std::vector< std::string >{"", "spam"}), incomingRequest.target.GetPath());
    EXPECT_EQ("www.example.com", incomingRequest.headers.GetHeaderValue("Host"));

    // Provide a response back to the client, in one piece.
    response = Http::Response();
    response.statusCode = 200;
    response.reasonPhrase = "OK";
    response.headers.SetHeader("Foo", "FeelsBadMan");
    response.headers.SetHeader("Content-Type", "text/plain");
    response.headers.SetHeader("Content-Length", "7");
    response.body = "HeyGuys";
    responseEncoding = response.Generate();
    connection->dataReceivedDelegate({responseEncoding.begin(), responseEncoding.end()});

    // Wait for client transaction to complete.
    ASSERT_TRUE(transaction->AwaitCompletion(std::chrono::milliseconds(100)));
    EXPECT_EQ(200, transaction->response.statusCode);
    EXPECT_EQ("OK", transaction->response.reasonPhrase);
    EXPECT_EQ("FeelsBadMan", transaction->response.headers.GetHeaderValue("Foo"));
    EXPECT_EQ("text/plain", transaction->response.headers.GetHeaderValue("Content-Type"));
    EXPECT_EQ("7", transaction->response.headers.GetHeaderValue("Content-Length"));
    EXPECT_EQ("HeyGuys", transaction->response.body);
}

TEST_F(ClientTests, SimpleGetRequestConnectionBrokenByServerBeforeResponseCompleted) {
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
    ASSERT_TRUE(transport->AwaitConnections(1));
    const auto connection = transport->connections[0];

    // Break the connection to the client.
    ASSERT_FALSE(connection->brokenDelegate == nullptr);
    connection->brokenDelegate(false);

    // Wait for client transaction to complete.
    // Expect a special status code indicating the server is unavailable.
    ASSERT_TRUE(transaction->AwaitCompletion(std::chrono::milliseconds(100)));
    EXPECT_EQ(Http::Client::Transaction::State::Broken, transaction->state);
}

TEST_F(ClientTests, SimpleGetRequestConnectionRefused) {
    // Set up the client.
    const auto transport = std::make_shared< MockTransport >();
    transport->connectionsAllowed = 0;
    Http::Client::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    client.Mobilize(deps);

    // Have the client make a simple request.
    Http::Request outgoingRequest;
    outgoingRequest.method = "GET";
    outgoingRequest.target.ParseFromString("http://www.example.com:1234/foo");
    const auto transaction = client.Request(outgoingRequest);

    // Wait for client transaction to complete.
    // Expect a special status code indicating the server is unavailable.
    ASSERT_TRUE(transaction->AwaitCompletion(std::chrono::milliseconds(100)));
    EXPECT_EQ(Http::Client::Transaction::State::UnableToConnect, transaction->state);
}

TEST_F(ClientTests, BrokenPersistentConnectionNotReused) {
    // Set up the client.
    const auto transport = std::make_shared< MockTransport >();
    transport->connectionsAllowed = 2;
    Http::Client::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    client.Mobilize(deps);

    // Have the client make a simple request, using a persistent connection.
    Http::Request outgoingRequest;
    outgoingRequest.method = "GET";
    outgoingRequest.target.ParseFromString("http://www.example.com:1234/foo");
    auto transaction = client.Request(outgoingRequest, true);
    auto connection = transport->connections[0];
    auto incomingRequest = connection->requests[0];
    EXPECT_FALSE(connection->broken);
    EXPECT_FALSE(incomingRequest.headers.HasHeaderToken("Connection", "Close"));

    // Break the connection from the server end.
    ASSERT_FALSE(connection->brokenDelegate == nullptr);
    connection->brokenDelegate(false);

    // Wait for client transaction to complete.
    ASSERT_TRUE(transaction->AwaitCompletion(std::chrono::milliseconds(100)));

    // Have the client make another simple request to the same server.
    // Expect a new connection to be made.
    outgoingRequest = Http::Request();
    outgoingRequest.method = "GET";
    outgoingRequest.target.ParseFromString("http://www.example.com:1234/bar");
    transaction = client.Request(outgoingRequest);
    ASSERT_FALSE(transaction == nullptr);
    ASSERT_TRUE(transport->AwaitConnections(2));
    connection = transport->connections[1];
    ASSERT_TRUE(connection->AwaitRequests(1));
    incomingRequest = connection->requests[0];
    EXPECT_EQ("GET", incomingRequest.method);
    EXPECT_EQ((std::vector< std::string >{"", "bar"}), incomingRequest.target.GetPath());
    EXPECT_EQ("www.example.com", incomingRequest.headers.GetHeaderValue("Host"));
}

TEST_F(ClientTests, ResponseTimeoutPersistentConnectionNotReused) {
    // Set up the client.
    const auto transport = std::make_shared< MockTransport >();
    transport->connectionsAllowed = 2;
    const auto timeKeeper = std::make_shared< MockTimeKeeper >();
    Http::Client::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = timeKeeper;
    deps.requestTimeoutSeconds = 1.0;
    client.Mobilize(deps);

    // Have the client make a simple request, using a persistent connection.
    Http::Request outgoingRequest;
    outgoingRequest.method = "GET";
    outgoingRequest.target.ParseFromString("http://www.example.com:1234/foo");
    auto transaction = client.Request(outgoingRequest, true);
    auto connection = transport->connections[0];
    auto incomingRequest = connection->requests[0];
    EXPECT_FALSE(connection->broken);
    EXPECT_FALSE(incomingRequest.headers.HasHeaderToken("Connection", "Close"));

    // Allow enough time to pass such that the request will time out.
    timeKeeper->currentTime = 1.5;

    // Wait for client transaction to complete, expecting a timeout.
    ASSERT_TRUE(transaction->AwaitCompletion(std::chrono::milliseconds(100)));
    EXPECT_EQ(Http::Client::Transaction::State::Timeout, transaction->state);

    // Have the client make another simple request to the same server.
    // Expect a new connection to be made.
    outgoingRequest = Http::Request();
    outgoingRequest.method = "GET";
    outgoingRequest.target.ParseFromString("http://www.example.com:1234/bar");
    transaction = client.Request(outgoingRequest);
    ASSERT_FALSE(transaction == nullptr);
    ASSERT_TRUE(transport->AwaitConnections(2));
    connection = transport->connections[1];
    ASSERT_TRUE(connection->AwaitRequests(1));
    incomingRequest = connection->requests[0];
    EXPECT_EQ("GET", incomingRequest.method);
    EXPECT_EQ((std::vector< std::string >{"", "bar"}), incomingRequest.target.GetPath());
    EXPECT_EQ("www.example.com", incomingRequest.headers.GetHeaderValue("Host"));
}

TEST_F(ClientTests, ResponseTimeoutNotPersistentConnection) {
    // Set up the client.
    const auto transport = std::make_shared< MockTransport >();
    transport->connectionsAllowed = 2;
    const auto timeKeeper = std::make_shared< MockTimeKeeper >();
    Http::Client::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = timeKeeper;
    deps.requestTimeoutSeconds = 1.0;
    client.Mobilize(deps);

    // Have the client make a simple request, not using a persistent
    // connection.
    Http::Request outgoingRequest;
    outgoingRequest.method = "GET";
    outgoingRequest.target.ParseFromString("http://www.example.com:1234/foo");
    auto transaction = client.Request(outgoingRequest, false);
    auto connection = transport->connections[0];
    auto incomingRequest = connection->requests[0];
    EXPECT_FALSE(connection->broken);
    EXPECT_TRUE(incomingRequest.headers.HasHeaderToken("Connection", "Close"));

    // Allow enough time to pass such that the request will time out.
    timeKeeper->currentTime = 1.5;

    // Wait for client transaction to complete, expecting a timeout.
    ASSERT_TRUE(transaction->AwaitCompletion(std::chrono::milliseconds(100)));
    EXPECT_EQ(Http::Client::Transaction::State::Timeout, transaction->state);
}

TEST_F(ClientTests, ReceiveWholeBodyForResponseWithoutContentLengthOrTransferCoding) {
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
    auto transaction = client.Request(outgoingRequest, false);
    auto connection = transport->connections[0];
    connection->sendDataJustBeforeBreak = true;
    const auto& incomingRequest = connection->requests[0];
    EXPECT_TRUE(incomingRequest.headers.HasHeaderToken("Connection", "Close"));

    // Provide a response back to the client, in two pieces:
    // 1) status line and headers
    // 2) body
    Http::Response response;
    response.statusCode = 200;
    response.reasonPhrase = "OK";
    response.headers.SetHeader("Foo", "Bar");
    response.headers.SetHeader("Connection", "Close");
    response.headers.SetHeader("Content-Type", "text/plain");
    response.body = "PogChamp";
    const auto& responseEncoding = response.Generate();
    connection->dataReceivedDelegate({responseEncoding.begin(), responseEncoding.end() - response.body.length()});
    ASSERT_FALSE(transaction->AwaitCompletion(std::chrono::milliseconds(100)));
    connection->dataReceivedDelegate({responseEncoding.end() - response.body.length(), responseEncoding.end()});
    ASSERT_FALSE(transaction->AwaitCompletion(std::chrono::milliseconds(100)));
    connection->brokenDelegate(false);

    // Wait for client transaction to complete.
    ASSERT_TRUE(transaction->AwaitCompletion(std::chrono::milliseconds(100)));
    EXPECT_EQ(Http::Client::Transaction::State::Completed, transaction->state);
    EXPECT_EQ(200, transaction->response.statusCode);
    EXPECT_EQ("OK", transaction->response.reasonPhrase);
    EXPECT_EQ("Bar", transaction->response.headers.GetHeaderValue("Foo"));
    EXPECT_EQ("text/plain", transaction->response.headers.GetHeaderValue("Content-Type"));
    EXPECT_EQ("8", transaction->response.headers.GetHeaderValue("Content-Length"));
    EXPECT_EQ("PogChamp", transaction->response.body);

    // Wait for the client to close their end of the connection.
    EXPECT_TRUE(connection->broken);
    EXPECT_FALSE(connection->brokenGracefully);

    // Release all strong connection references, and verify the connection is not yet
    // destroyed (through a global flag which causes the connection destructor to
    // crash the test).
    crashOnConnectionDestruction = true;
    connection = nullptr;
    transport->connections.clear();

    // Release the transaction, and verify the connection is finally destroyed.
    crashOnConnectionDestruction = false;
    connectionDestroyed = false;
    transaction = nullptr;
    EXPECT_TRUE(connectionDestroyed);
}

TEST_F(ClientTests, GetRequestGzippedByServerUngzippedByClient) {
    // Set up the client.
    const auto transport = std::make_shared< MockTransport >();
    Http::Client::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    client.Mobilize(deps);

    // Have the client make a simple request.
    Http::Request outgoingRequest;
    outgoingRequest.method = "GET";
    outgoingRequest.target.ParseFromString("http://PePe@www.example.com:1234/foo?abc#def");
    const auto transaction = client.Request(outgoingRequest);
    (void)transport->AwaitConnections(1);
    const auto& connection = transport->connections[0];
    (void)connection->AwaitRequests(1);
    const auto& incomingRequest = connection->requests[0];
    EXPECT_EQ("gzip, deflate", incomingRequest.headers.GetHeaderValue("Accept-Encoding"));

    // Provide a response back to the client, in one piece,
    // applying gzip compression.
    Http::Response response;
    response.statusCode = 200;
    response.reasonPhrase = "OK";
    response.headers.SetHeader("Foo", "Bar");
    response.headers.SetHeader("Content-Type", "text/plain");
    response.headers.SetHeader("Content-Encoding", "gzip");
    response.headers.SetHeader("Vary", "Accept-Encoding");
    response.headers.SetHeader("Content-Length", "33");
    response.body = std::string(
        "\x1F\x8B\x08\x00\x00\x00\x00\x00\x00\x0A\xF3\x48\xCD\xC9\xC9\xD7"
        "\x51\x08\xCF\x2F\xCA\x49\x51\x04\x00\xD0\xC3\x4A\xEC\x0D\x00\x00"
        "\x00",
        33
    );
    const auto& responseEncoding = response.Generate();
    connection->dataReceivedDelegate({responseEncoding.begin(), responseEncoding.end()});

    // Wait for client transaction to complete.
    ASSERT_TRUE(transaction->AwaitCompletion(std::chrono::milliseconds(100)));
    EXPECT_EQ(Http::Client::Transaction::State::Completed, transaction->state);
    EXPECT_EQ(200, transaction->response.statusCode);
    EXPECT_EQ("OK", transaction->response.reasonPhrase);
    EXPECT_EQ("Bar", transaction->response.headers.GetHeaderValue("Foo"));
    EXPECT_EQ("text/plain", transaction->response.headers.GetHeaderValue("Content-Type"));
    EXPECT_EQ("", transaction->response.headers.GetHeaderValue("Content-Encoding"));
    EXPECT_EQ("13", transaction->response.headers.GetHeaderValue("Content-Length"));
    EXPECT_EQ("Hello, World!", transaction->response.body);
}

TEST_F(ClientTests, SimpleGetRequestFragmentedChunkedGzippedResponse) {
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
    EXPECT_FALSE(incomingRequest.target.HasQuery());;
    EXPECT_FALSE(incomingRequest.target.HasFragment());

    // Provide a response back to the client, in fragments.
    const std::string responseEncoding(
        "HTTP/1.1 200 OK\r\n"
        "Foo: Bar\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Encoding: gzip\r\n"
        "Vary: Accept-Encoding\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "21\r\n"
        "\x1F\x8B\x08\x00\x00\x00\x00\x00\x00\x0A\xF3\x48\xCD\xC9\xC9\xD7"
        "\x51\x08\xCF\x2F\xCA\x49\x51\x04\x00\xD0\xC3\x4A\xEC\x0D\x00\x00"
        "\x00\r\n"
        "0\r\n"
        "\r\n",
        174
    );
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
    EXPECT_EQ("13", transaction->response.headers.GetHeaderValue("Content-Length"));
    EXPECT_EQ("Hello, World!", transaction->response.body);
}

TEST_F(ClientTests, GetRequestGzippedByServerEmpty) {
    // Set up the client.
    const auto transport = std::make_shared< MockTransport >();
    Http::Client::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    client.Mobilize(deps);

    // Have the client make a simple request.
    Http::Request outgoingRequest;
    outgoingRequest.method = "GET";
    outgoingRequest.target.ParseFromString("http://PePe@www.example.com:1234/foo?abc#def");
    const auto transaction = client.Request(outgoingRequest);
    (void)transport->AwaitConnections(1);
    const auto& connection = transport->connections[0];
    (void)connection->AwaitRequests(1);
    const auto& incomingRequest = connection->requests[0];
    EXPECT_EQ("gzip, deflate", incomingRequest.headers.GetHeaderValue("Accept-Encoding"));

    // Provide a response back to the client, in one piece,
    // applying gzip compression, but give an empty (invalid) body.
    Http::Response response;
    response.statusCode = 200;
    response.reasonPhrase = "OK";
    response.headers.SetHeader("Foo", "Bar");
    response.headers.SetHeader("Content-Type", "text/plain");
    response.headers.SetHeader("Content-Encoding", "gzip");
    response.headers.SetHeader("Vary", "Accept-Encoding");
    response.headers.SetHeader("Content-Length", "0");
    response.body = "";
    const auto& responseEncoding = response.Generate();
    connection->dataReceivedDelegate({responseEncoding.begin(), responseEncoding.end()});

    // Wait for client transaction to complete.
    ASSERT_TRUE(transaction->AwaitCompletion(std::chrono::milliseconds(100)));
    EXPECT_EQ(Http::Response::State::Error, transaction->response.state);
}

TEST_F(ClientTests, GetRequestGzippedByServerJunk) {
    // Set up the client.
    const auto transport = std::make_shared< MockTransport >();
    Http::Client::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    client.Mobilize(deps);

    // Have the client make a simple request.
    Http::Request outgoingRequest;
    outgoingRequest.method = "GET";
    outgoingRequest.target.ParseFromString("http://PePe@www.example.com:1234/foo?abc#def");
    const auto transaction = client.Request(outgoingRequest);
    (void)transport->AwaitConnections(1);
    const auto& connection = transport->connections[0];
    (void)connection->AwaitRequests(1);
    const auto& incomingRequest = connection->requests[0];
    EXPECT_EQ("gzip, deflate", incomingRequest.headers.GetHeaderValue("Accept-Encoding"));

    // Provide a response back to the client, in one piece,
    // applying gzip compression, but give an empty (invalid) body.
    Http::Response response;
    response.statusCode = 200;
    response.reasonPhrase = "OK";
    response.headers.SetHeader("Foo", "Bar");
    response.headers.SetHeader("Content-Type", "text/plain");
    response.headers.SetHeader("Content-Encoding", "gzip");
    response.headers.SetHeader("Vary", "Accept-Encoding");
    response.headers.SetHeader("Content-Length", "42");
    response.body = "Hello, this is certainly not gzipped data!";
    const auto& responseEncoding = response.Generate();
    connection->dataReceivedDelegate({responseEncoding.begin(), responseEncoding.end()});

    // Wait for client transaction to complete.
    ASSERT_TRUE(transaction->AwaitCompletion(std::chrono::milliseconds(100)));
    EXPECT_EQ(Http::Response::State::Error, transaction->response.state);
}

TEST_F(ClientTests, GetRequestGzippedByServerUngzippedByClientEmpty) {
    // Set up the client.
    const auto transport = std::make_shared< MockTransport >();
    Http::Client::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    client.Mobilize(deps);

    // Have the client make a simple request.
    Http::Request outgoingRequest;
    outgoingRequest.method = "GET";
    outgoingRequest.target.ParseFromString("http://PePe@www.example.com:1234/foo?abc#def");
    const auto transaction = client.Request(outgoingRequest);
    (void)transport->AwaitConnections(1);
    const auto& connection = transport->connections[0];
    (void)connection->AwaitRequests(1);
    const auto& incomingRequest = connection->requests[0];
    EXPECT_EQ("gzip, deflate", incomingRequest.headers.GetHeaderValue("Accept-Encoding"));

    // Provide a response back to the client, in one piece,
    // applying gzip compression.
    Http::Response response;
    response.statusCode = 200;
    response.reasonPhrase = "OK";
    response.headers.SetHeader("Foo", "Bar");
    response.headers.SetHeader("Content-Type", "text/plain");
    response.headers.SetHeader("Content-Encoding", "gzip");
    response.headers.SetHeader("Vary", "Accept-Encoding");
    response.headers.SetHeader("Content-Length", "29");
    response.body = std::string(
        "\x1f\x8b\x08\x08\x2d\xac\xca\x5b\x00\x03\x74\x65\x73\x74\x2e\x74"
        "\x78\x74\x00\x03\x00\x00\x00\x00\x00\x00\x00\x00\x00",
        29
    );
    const auto& responseEncoding = response.Generate();
    connection->dataReceivedDelegate({responseEncoding.begin(), responseEncoding.end()});

    // Wait for client transaction to complete.
    ASSERT_TRUE(transaction->AwaitCompletion(std::chrono::milliseconds(100)));
    EXPECT_EQ(Http::Client::Transaction::State::Completed, transaction->state);
    EXPECT_EQ(200, transaction->response.statusCode);
    EXPECT_EQ("OK", transaction->response.reasonPhrase);
    EXPECT_EQ("Bar", transaction->response.headers.GetHeaderValue("Foo"));
    EXPECT_EQ("text/plain", transaction->response.headers.GetHeaderValue("Content-Type"));
    EXPECT_EQ("", transaction->response.headers.GetHeaderValue("Content-Encoding"));
    EXPECT_EQ("0", transaction->response.headers.GetHeaderValue("Content-Length"));
    EXPECT_EQ("", transaction->response.body);
}

TEST_F(ClientTests, SecondRequestPersistentSameServerOpensSecondConnection) {
    // Set up the client.
    const auto transport = std::make_shared< MockTransport >();
    transport->connectionsAllowed = 2;
    Http::Client::MobilizationDependencies deps;
    deps.transport = transport;
    deps.timeKeeper = std::make_shared< MockTimeKeeper >();
    client.Mobilize(deps);

    // Have the client make a simple request, using a persistent connection.
    Http::Request outgoingRequest;
    outgoingRequest.method = "GET";
    outgoingRequest.target.ParseFromString("http://www.example.com:1234/foo");
    auto transaction1 = client.Request(outgoingRequest, true);
    auto connection1 = transport->connections[0];

    // Have the client make the same request a second time, using a persistent
    // connection as well.
    auto transaction2 = client.Request(outgoingRequest, true);
    ASSERT_EQ(2, transport->connections.size());
    auto connection2 = transport->connections[1];

    // Provide a response back to the client for the first transaction, in one
    // piece.
    Http::Response response;
    response.statusCode = 200;
    response.reasonPhrase = "OK";
    response.headers.SetHeader("Foo", "Bar");
    response.headers.SetHeader("Content-Type", "text/plain");
    response.headers.SetHeader("Content-Length", "8");
    response.body = "PogChamp";
    auto responseEncoding = response.Generate();
    connection1->dataReceivedDelegate({responseEncoding.begin(), responseEncoding.end()});

    // Provide a response back to the client for the second transaction, in one
    // piece.
    response.headers.SetHeader("Content-Length", "7");
    response.body = "Poggers";
    responseEncoding = response.Generate();
    connection2->dataReceivedDelegate({responseEncoding.begin(), responseEncoding.end()});

    // Wait for client transactions to complete.
    ASSERT_TRUE(transaction1->AwaitCompletion(std::chrono::milliseconds(100)));
    ASSERT_TRUE(transaction2->AwaitCompletion(std::chrono::milliseconds(100)));
    EXPECT_EQ("PogChamp", transaction1->response.body);
    EXPECT_EQ("Poggers", transaction2->response.body);
}
