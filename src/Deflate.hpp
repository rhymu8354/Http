#ifndef HTTP_DEFLATE_HPP
#define HTTP_DEFLATE_HPP

/**
 * @file Deflate.hpp
 *
 * This module declares the Http::Deflate function and its friends.
 *
 * © 2018 by Richard Walters
 */

#include <stdint.h>
#include <string>
#include <vector>

namespace Http {

    /**
     * This is used to pick a compression mode for the Deflate method.
     */
    enum class DeflateMode {
        /**
         * This selects the "zlib" data format
         * [RFC1950](https://tools.ietf.org/html/rfc1950) containing a
         * "deflate" compressed data stream
         * [RFC1951](https://tools.ietf.org/html/rfc1951) that uses a
         * combination of the Lempel-Ziv (LZ77) compression algorithm and
         * Huffman coding.
         */
        Deflate,

        /**
         * This selects the "gzip" data format, which is an LZ77 coding with a
         * 32-bit Cyclic Redundancy Check (CRC) that is commonly produced by
         * the gzip file compression program
         * [RFC1952](https://tools.ietf.org/html/rfc1952).
         */
        Gzip,
    };

    /**
     * This function compresses the given vector of bytes, using
     * the given compression mode.
     *
     * @param[in] input
     *     These are the bytes to compress.
     *
     * @param[in] mode
     *     This identifies the compression scheme to use.
     */
    std::vector< uint8_t > Deflate(
        const std::vector< uint8_t >& input,
        DeflateMode mode
    );

    /**
     * This function compresses the given string, using
     * the given compression mode.
     *
     * @param[in] input
     *     These are the bytes to compress.
     *
     * @param[in] mode
     *     This identifies the compression scheme to use.
     */
    std::string Deflate(
        const std::string& input,
        DeflateMode mode
    );

}

#endif /* HTTP_DEFLATE_HPP */
