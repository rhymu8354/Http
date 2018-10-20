/**
 * @file Inflate.cpp
 *
 * This module contains the implementation of the Http::Inflate function.
 *
 * © 2018 by Richard Walters
 */

#include "Inflate.hpp"

#include <functional>
#include <memory>
#include <stddef.h>
#include <vector>
#include <zlib.h>

namespace {

    /**
     * This is the number of bytes that we will allocate at a time while
     * inflating data.
     */
    constexpr size_t INFLATE_BUFFER_INCREMENT = 65536;

}

namespace Http {

    bool Inflate(
        const std::vector< uint8_t >& input,
        std::vector< uint8_t >& output,
        InflateMode mode
    ) {
        // Initialize the Inflate stream.
        output.clear();
        z_stream inflateStream;
        inflateStream.zalloc = Z_NULL;
        inflateStream.zfree = Z_NULL;
        inflateStream.opaque = Z_NULL;
        if (mode == InflateMode::Ungzip) {
            if (
                inflateInit2(
                    &inflateStream,
                    16 + MAX_WBITS
                ) != Z_OK
            ) {
                return false;
            }
        } else {
            if (inflateInit(&inflateStream) != Z_OK) {
                return false;
            }
        }

        // Make a dummy object that will be used to clean up the Inflate stream
        // when the function returns, no matter at what point it returns.
        std::unique_ptr< z_stream, std::function< void(z_stream*) > > inflateStreamReference(
            &inflateStream,
            [](z_stream* z) {
                inflateEnd(z);
            }
        );

        // Inflate the data.
        inflateStream.next_in = (Bytef*)input.data();
        inflateStream.avail_in = (uInt)input.size();
        inflateStream.total_in = 0;
        int result = Z_OK;
        while (result != Z_STREAM_END) {
            size_t totalInflatedPreviously = output.size();
            output.resize(totalInflatedPreviously + INFLATE_BUFFER_INCREMENT);
            inflateStream.next_out = (Bytef*)output.data() + totalInflatedPreviously;
            inflateStream.avail_out = INFLATE_BUFFER_INCREMENT;
            inflateStream.total_out = 0;
            result = inflate(&inflateStream, Z_FINISH);
            output.resize(totalInflatedPreviously + (size_t)inflateStream.total_out);
            if (
                (result == Z_BUF_ERROR)
                && (inflateStream.total_out == 0)
            ) {
                return false;
            } else if (
                (result != Z_OK)
                && (result != Z_STREAM_END)
                && (result != Z_BUF_ERROR)
            ) {
                return false;
            }
        }
        return true;
    }

    bool Inflate(
        const std::string& input,
        std::string& output,
        InflateMode mode
    ) {
        std::vector< uint8_t > outputBytes;
        if (
            !Inflate(
                std::vector< uint8_t >{input.begin(), input.end()},
                outputBytes,
                mode
            )
        ) {
            return false;
        }
        output = std::string{outputBytes.begin(), outputBytes.end()};
        return true;
    }

}
