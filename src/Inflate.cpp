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

    std::vector< uint8_t > Inflate(
        const std::vector< uint8_t >& input,
        InflateMode mode
    ) {
        // Initialize the Inflate stream.
        std::vector< uint8_t > output;
        z_stream InflateStream;
        InflateStream.zalloc = Z_NULL;
        InflateStream.zfree = Z_NULL;
        InflateStream.opaque = Z_NULL;
        if (mode == InflateMode::Ungzip) {
            if (
                inflateInit2(
                    &InflateStream,
                    16 + MAX_WBITS
                ) != Z_OK
            ) {
                return {};
            }
        } else {
            if (inflateInit(&InflateStream) != Z_OK) {
                return {};
            }
        }

        // Make a dummy object that will be used to clean up the Inflate stream
        // when the function returns, no matter at what point it returns.
        std::unique_ptr< z_stream, std::function< void(z_stream*) > > InflateStreamReference(
            &InflateStream,
            [](z_stream* z) {
                inflateEnd(z);
            }
        );

        // Inflate the data.
        InflateStream.next_in = (Bytef*)input.data();
        InflateStream.avail_in = (uInt)input.size();
        InflateStream.total_in = 0;
        int result = Z_OK;
        while (result != Z_STREAM_END) {
            size_t totalInflatedPreviously = output.size();
            output.resize(totalInflatedPreviously + INFLATE_BUFFER_INCREMENT);
            InflateStream.next_out = (Bytef*)output.data() + totalInflatedPreviously;
            InflateStream.avail_out = INFLATE_BUFFER_INCREMENT;
            InflateStream.total_out = 0;
            result = inflate(&InflateStream, Z_FINISH);
            output.resize(totalInflatedPreviously + (size_t)InflateStream.total_out);
            if (
                (result != Z_OK)
                && (result != Z_STREAM_END)
                && (result != Z_BUF_ERROR) // TODO: check if we need to ignore Z_BUF_ERROR
            ) {
                return {};
            }
        }

        // Return Inflated data.
        return output;
    }

    std::string Inflate(
        const std::string& input,
        InflateMode mode
    ) {
        const auto output = Inflate(
            std::vector< uint8_t >{input.begin(), input.end()},
            mode
        );
        return std::string{output.begin(), output.end()};
    }

}
