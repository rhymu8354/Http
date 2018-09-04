/**
 * @file ChunkedBodyTests.cpp
 *
 * This module contains the unit tests of the
 * Http::ChunkedBody class.
 *
 * © 2018 by Richard Walters
 */

#include <condition_variable>
#include <gtest/gtest.h>
#include <Http/ChunkedBody.hpp>
#include <inttypes.h>
#include <memory>
#include <mutex>
#include <stdlib.h>
#include <SystemAbstractions/DiagnosticsSender.hpp>
#include <SystemAbstractions/StringExtensions.hpp>

/**
 * This is the test fixture for these tests, providing common
 * setup and teardown for each test.
 */
struct ChunkedBodyTests
    : public ::testing::Test
{
    // Properties

    /**
     * This is the unit under test.
     */
    Http::ChunkedBody body;

    // Methods

    // ::testing::Test

    virtual void SetUp() override {
    }

    virtual void TearDown() override {
    }
};

TEST_F(ChunkedBodyTests, DecodeSimpleEmptyBodyOnePiece) {
    ASSERT_EQ(5, body.Decode("0\r\n\r\n"));
    ASSERT_EQ(Http::ChunkedBody::State::Complete, body.GetState());
    ASSERT_EQ("", (std::string)body);
}

TEST_F(ChunkedBodyTests, DecodeEmptyBodyMultipleZeroes) {
    ASSERT_EQ(9, body.Decode("00000\r\n\r\n"));
    ASSERT_EQ(Http::ChunkedBody::State::Complete, body.GetState());
    ASSERT_EQ("", (std::string)body);
}

TEST_F(ChunkedBodyTests, DecodeEmptyBodyWithChunkExtensionNoValue) {
    ASSERT_EQ(12, body.Decode("000;dude\r\n\r\n"));
    ASSERT_EQ(Http::ChunkedBody::State::Complete, body.GetState());
    ASSERT_EQ("", (std::string)body);
}

TEST_F(ChunkedBodyTests, DecodeEmptyBodyWithChunkExtensionWithUnquotedValue) {
    ASSERT_EQ(22, body.Decode("000;Kappa=PogChamp\r\n\r\n"));
    ASSERT_EQ(Http::ChunkedBody::State::Complete, body.GetState());
    ASSERT_EQ("", (std::string)body);
}

TEST_F(ChunkedBodyTests, DecodeEmptyBodyWithChunkExtensionWithQuotedValue) {
    ASSERT_EQ(29, body.Decode("000;Kappa=\"Hello, World!\"\r\n\r\n"));
    ASSERT_EQ(Http::ChunkedBody::State::Complete, body.GetState());
    ASSERT_EQ("", (std::string)body);
}

TEST_F(ChunkedBodyTests, DecodeEmptyBodyWithMultipleChunkExtensions) {
    ASSERT_EQ(49, body.Decode("000;Foo=Bar;Kappa=\"Hello, World!\";Spam=12345!\r\n\r\n"));
    ASSERT_EQ(Http::ChunkedBody::State::Complete, body.GetState());
    ASSERT_EQ("", (std::string)body);
}

TEST_F(ChunkedBodyTests, DecodeSimpleEmptyBodyOneCharacterAtATime) {
    const std::string input = "0\r\n\r\n";
    size_t index = 0;
    for (auto c: input) {
        ASSERT_EQ(1, body.Decode(std::string(1, c))) << index;
        ++index;
        if (index < 3) {
            ASSERT_EQ(Http::ChunkedBody::State::DecodingChunks, body.GetState()) << index;
        } else if (index < 5) {
            ASSERT_EQ(Http::ChunkedBody::State::DecodingTrailer, body.GetState()) << index;
        } else {
            ASSERT_EQ(Http::ChunkedBody::State::Complete, body.GetState()) << index;
        }
    }
    ASSERT_EQ("", (std::string)body);
}

TEST_F(ChunkedBodyTests, DecodeSimpleEmptyBodyOnePieceWithExtraStuffAfter) {
    ASSERT_EQ(5, body.Decode("0\r\n\r\nHello!"));
    ASSERT_EQ(Http::ChunkedBody::State::Complete, body.GetState());
    ASSERT_EQ("", (std::string)body);
}

TEST_F(ChunkedBodyTests, DecodeSimpleEmptyBodyTwoPiecesSubstring) {
    ASSERT_EQ(4, body.Decode("XYZ0\r\n\r\n123", 3, 4));
    ASSERT_EQ(Http::ChunkedBody::State::DecodingTrailer, body.GetState());
    ASSERT_EQ(1, body.Decode("XYZ0\r\n\r\n123", 7, 2));
    ASSERT_EQ(Http::ChunkedBody::State::Complete, body.GetState());
    ASSERT_EQ("", (std::string)body);
}

TEST_F(ChunkedBodyTests, DecodeSimpleNonEmptyBodyOnePiece) {
    ASSERT_EQ(15, body.Decode("5\r\nHello\r\n0\r\n\r\n"));
    ASSERT_EQ(Http::ChunkedBody::State::Complete, body.GetState());
    ASSERT_EQ("Hello", (std::string)body);
}

TEST_F(ChunkedBodyTests, DecodeSimpleNonEmptyBodyOneCharacterAtATime) {
    const std::string input = "5\r\nHello\r\n0\r\n\r\n";
    size_t index = 0;
    for (auto c: input) {
        ASSERT_EQ(1, body.Decode(std::string(1, c))) << index;
        ++index;
        if (index < 3) {
            ASSERT_EQ(Http::ChunkedBody::State::DecodingChunks, body.GetState()) << index;
        } else if (index < 8) {
            ASSERT_EQ(Http::ChunkedBody::State::ReadingChunkData, body.GetState()) << index;
        } else if (index < 10) {
            ASSERT_EQ(Http::ChunkedBody::State::ReadingChunkDelimiter, body.GetState()) << index;
        } else if (index < 13) {
            ASSERT_EQ(Http::ChunkedBody::State::DecodingChunks, body.GetState()) << index;
        } else if (index < 15) {
            ASSERT_EQ(Http::ChunkedBody::State::DecodingTrailer, body.GetState()) << index;
        } else {
            ASSERT_EQ(Http::ChunkedBody::State::Complete, body.GetState()) << index;
        }
    }
    ASSERT_EQ("Hello", (std::string)body);
}
