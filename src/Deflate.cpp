/**
 * @file Deflate.cpp
 *
 * This module contains the implementation of the Http::Deflate function.
 *
 * © 2018 by Richard Walters
 */

#include "Deflate.hpp"

#include <functional>
#include <memory>
#include <stddef.h>
#include <vector>
#include <zlib.h>

namespace {

    /**
     * This is the number of bytes that we will allocate at a time while
     * deflating data.
     */
    constexpr size_t DEFLATE_BUFFER_INCREMENT = 65536;

}

namespace Http {

    std::vector< uint8_t > Deflate(
        const std::vector< uint8_t >& input,
        DeflateMode mode
    ) {
        // Initialize the deflate stream.
        std::vector< uint8_t > output;
        z_stream deflateStream;
        deflateStream.zalloc = Z_NULL;
        deflateStream.zfree = Z_NULL;
        deflateStream.opaque = Z_NULL;
        if (mode == DeflateMode::Gzip) {
            if (
                deflateInit2(
                    &deflateStream,
                    Z_DEFAULT_COMPRESSION,
                    Z_DEFLATED,
                    16 + MAX_WBITS,
                    8,
                    Z_DEFAULT_STRATEGY
                ) != Z_OK
            ) {
                return {};
            }
        } else {
            if (deflateInit(&deflateStream, Z_DEFAULT_COMPRESSION) != Z_OK) {
                return {};
            }
        }

        // Make a dummy object that will be used to clean up the deflate stream
        // when the function returns, no matter at what point it returns.
        std::unique_ptr< z_stream, std::function< void(z_stream*) > > deflateStreamReference(
            &deflateStream,
            [](z_stream* z) {
                deflateEnd(z);
            }
        );

        // Deflate the data.
        deflateStream.next_in = (Bytef*)input.data();
        deflateStream.avail_in = (uInt)input.size();
        deflateStream.total_in = 0;
        int result = Z_OK;
        while (result != Z_STREAM_END) {
            size_t totalDeflatedPreviously = output.size();
            output.resize(totalDeflatedPreviously + DEFLATE_BUFFER_INCREMENT);
            deflateStream.next_out = (Bytef*)output.data() + totalDeflatedPreviously;
            deflateStream.avail_out = DEFLATE_BUFFER_INCREMENT;
            deflateStream.total_out = 0;
            result = deflate(&deflateStream, Z_FINISH);
            output.resize(totalDeflatedPreviously + (size_t)deflateStream.total_out);
            if (
                (result == Z_BUF_ERROR)
                && (deflateStream.total_out == 0)
            ) {
                return {};
            } else if (
                (result != Z_OK)
                && (result != Z_STREAM_END)
                && (result != Z_BUF_ERROR)
            ) {
                return {};
            }
        }

        // Return deflated data.
        return output;
    }

    std::string Deflate(
        const std::string& input,
        DeflateMode mode
    ) {
        const auto output = Deflate(
            std::vector< uint8_t >{input.begin(), input.end()},
            mode
        );
        return std::string{output.begin(), output.end()};
    }

}
