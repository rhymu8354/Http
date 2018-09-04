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
    size_t accepted = 0;
    for (size_t i = 0; i < input.length(); ++i) {
        accepted += body.Decode(input.substr(accepted, i + 1 - accepted));
        if (i < 2) {
            ASSERT_EQ(Http::ChunkedBody::State::DecodingChunks, body.GetState()) << i;
            ASSERT_EQ(i + 1, accepted);
        } else if (i < 4) {
            ASSERT_EQ(Http::ChunkedBody::State::DecodingTrailer, body.GetState()) << i;
            ASSERT_EQ(3, accepted);
        } else {
            ASSERT_EQ(Http::ChunkedBody::State::Complete, body.GetState()) << i;
            ASSERT_EQ(5, accepted);
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
    ASSERT_EQ(3, body.Decode("XYZ0\r\n\r\n123", 3, 4));
    ASSERT_EQ(Http::ChunkedBody::State::DecodingTrailer, body.GetState());
    ASSERT_EQ(2, body.Decode("XYZ0\r\n\r\n123", 6, 3));
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
    size_t accepted = 0;
    for (size_t i = 0; i < input.length(); ++i) {
        accepted += body.Decode(input.substr(accepted, i + 1 - accepted));
        if (i < 2) {
            ASSERT_EQ(Http::ChunkedBody::State::DecodingChunks, body.GetState()) << i;
            ASSERT_EQ(i + 1, accepted);
        } else if (i < 7) {
            ASSERT_EQ(Http::ChunkedBody::State::ReadingChunkData, body.GetState()) << i;
            ASSERT_EQ(i + 1, accepted);
        } else if (i < 9) {
            ASSERT_EQ(Http::ChunkedBody::State::ReadingChunkDelimiter, body.GetState()) << i;
            ASSERT_EQ(i + 1, accepted);
        } else if (i < 12) {
            ASSERT_EQ(Http::ChunkedBody::State::DecodingChunks, body.GetState()) << i;
            ASSERT_EQ(i + 1, accepted);
        } else if (i < 14) {
            ASSERT_EQ(Http::ChunkedBody::State::DecodingTrailer, body.GetState()) << i;
            ASSERT_EQ(13, accepted);
        } else {
            ASSERT_EQ(Http::ChunkedBody::State::Complete, body.GetState()) << i;
            ASSERT_EQ(15, accepted);
        }
    }
    ASSERT_EQ("Hello", (std::string)body);
}

TEST_F(ChunkedBodyTests, DecodeTwoChunkBodyOnePiece) {
    ASSERT_EQ(28, body.Decode("6\r\nHello,\r\n7\r\n World!\r\n0\r\n\r\n"));
    ASSERT_EQ(Http::ChunkedBody::State::Complete, body.GetState());
    ASSERT_EQ("Hello, World!", (std::string)body);
}

TEST_F(ChunkedBodyTests, DecodeTwoChunkBodyOneCharacterAtATime) {
    const std::string input = "6\r\nHello,\r\n7\r\n World!\r\n0\r\n\r\n";
    size_t accepted = 0;
    for (size_t i = 0; i < input.length(); ++i) {
        accepted += body.Decode(input.substr(accepted, i + 1 - accepted));
        if (i < 2) {
            ASSERT_EQ(Http::ChunkedBody::State::DecodingChunks, body.GetState()) << i;
            ASSERT_EQ(i + 1, accepted);
        } else if (i < 8) {
            ASSERT_EQ(Http::ChunkedBody::State::ReadingChunkData, body.GetState()) << i;
            ASSERT_EQ(i + 1, accepted);
        } else if (i < 10) {
            ASSERT_EQ(Http::ChunkedBody::State::ReadingChunkDelimiter, body.GetState()) << i;
            ASSERT_EQ(i + 1, accepted);
        } else if (i < 13) {
            ASSERT_EQ(Http::ChunkedBody::State::DecodingChunks, body.GetState()) << i;
            ASSERT_EQ(i + 1, accepted);
        } else if (i < 20) {
            ASSERT_EQ(Http::ChunkedBody::State::ReadingChunkData, body.GetState()) << i;
            ASSERT_EQ(i + 1, accepted);
        } else if (i < 22) {
            ASSERT_EQ(Http::ChunkedBody::State::ReadingChunkDelimiter, body.GetState()) << i;
            ASSERT_EQ(i + 1, accepted);
        } else if (i < 25) {
            ASSERT_EQ(Http::ChunkedBody::State::DecodingChunks, body.GetState()) << i;
            ASSERT_EQ(i + 1, accepted);
        } else if (i < 27) {
            ASSERT_EQ(Http::ChunkedBody::State::DecodingTrailer, body.GetState()) << i;
            ASSERT_EQ(26, accepted);
        } else {
            ASSERT_EQ(Http::ChunkedBody::State::Complete, body.GetState()) << i;
            ASSERT_EQ(28, accepted);
        }
    }
    ASSERT_EQ("Hello, World!", (std::string)body);
}

TEST_F(ChunkedBodyTests, DecodeTrailersOnePiece) {
    EXPECT_EQ(41, body.Decode("0\r\nX-Foo: Bar\r\nX-Poggers: FeelsBadMan\r\n\r\n"));
    ASSERT_EQ(Http::ChunkedBody::State::Complete, body.GetState());
    EXPECT_EQ("", (std::string)body);
    const auto actualNumTrailers = body.GetTrailers().GetAll().size();
    struct ExpectedTrailer {
        std::string name;
        std::string value;
    };
    const std::vector< ExpectedTrailer > expectedTrailers{
        {"X-Foo", "Bar"},
        {"X-Poggers", "FeelsBadMan"},
    };
    EXPECT_EQ(expectedTrailers.size(), actualNumTrailers);
    for (const auto& expectedTrailer: expectedTrailers) {
        EXPECT_EQ(
            expectedTrailer.value,
            body.GetTrailers().GetHeaderValue(expectedTrailer.name)
        );
    }
}

TEST_F(ChunkedBodyTests, DecodeTrailersOneCharacterAtATime) {
    const std::string input = "0\r\nX-Foo: Bar\r\nX-Poggers: FeelsBadMan\r\n\r\n";
    size_t accepted = 0;
    for (size_t i = 0; i < input.length(); ++i) {
        accepted += body.Decode(input.substr(accepted, i + 1 - accepted));
        if (i < 2) {
            ASSERT_EQ(Http::ChunkedBody::State::DecodingChunks, body.GetState()) << i;
            ASSERT_EQ(i + 1, accepted) << i;
        } else if (i < 38) {
            ASSERT_EQ(Http::ChunkedBody::State::DecodingTrailer, body.GetState()) << i;
            ASSERT_EQ(3, accepted) << i;
        } else if (i < 40) {
            ASSERT_EQ(Http::ChunkedBody::State::DecodingTrailer, body.GetState()) << i;
            ASSERT_EQ(15, accepted) << i;
        } else {
            ASSERT_EQ(Http::ChunkedBody::State::Complete, body.GetState()) << i;
            ASSERT_EQ(41, accepted) << i;
        }
    }
    const auto actualNumTrailers = body.GetTrailers().GetAll().size();
    struct ExpectedTrailer {
        std::string name;
        std::string value;
    };
    const std::vector< ExpectedTrailer > expectedTrailers{
        {"X-Foo", "Bar"},
        {"X-Poggers", "FeelsBadMan"},
    };
    EXPECT_EQ(expectedTrailers.size(), actualNumTrailers);
    for (const auto& expectedTrailer: expectedTrailers) {
        EXPECT_EQ(
            expectedTrailer.value,
            body.GetTrailers().GetHeaderValue(expectedTrailer.name)
        );
    }
}
