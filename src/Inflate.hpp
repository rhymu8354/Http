#ifndef HTTP_INFLATE_HPP
#define HTTP_INFLATE_HPP

/**
 * @file Inflate.hpp
 *
 * This module declares the Http::Inflate function and its friends.
 *
 * © 2018 by Richard Walters
 */

#include <stdint.h>
#include <vector>

namespace Http {

    /**
     * This is used to pick a decompression mode for the Inflate method.
     */
    enum class InflateMode {
        /**
         * This selects the "zlib" data format
         * [RFC1950](https://tools.ietf.org/html/rfc1950) containing a
         * "Inflate" compressed data stream
         * [RFC1951](https://tools.ietf.org/html/rfc1951) that uses a
         * combination of the Lempel-Ziv (LZ77) compression algorithm and
         * Huffman coding.
         */
        Inflate,

        /**
         * This selects the "gzip" data format, which is an LZ77 coding with a
         * 32-bit Cyclic Redundancy Check (CRC) that is commonly produced by
         * the gzip file compression program
         * [RFC1952](https://tools.ietf.org/html/rfc1952).
         */
        Ungzip,
    };

    /**
     * This function decompresses the given vector of bytes, using
     * the given decompression mode.
     *
     * @param[in] input
     *     These are the bytes to decompress.
     *
     * @param[in] mode
     *     This identifies the decompression scheme to use.
     */
    std::vector< uint8_t > Inflate(
        const std::vector< uint8_t >& input,
        InflateMode mode
    );

    /**
     * This function decompresses the given string, using
     * the given decompression mode.
     *
     * @param[in] input
     *     These are the bytes to decompress.
     *
     * @param[in] mode
     *     This identifies the decompression scheme to use.
     */
    std::string Inflate(
        const std::string& input,
        InflateMode mode
    );

}

#endif /* HTTP_INFLATE_HPP */
