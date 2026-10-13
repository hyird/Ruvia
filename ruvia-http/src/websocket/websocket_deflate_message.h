#pragma once

#include <zlib.h>

#include <algorithm>
#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/http/websocket_deflate_codecs.h"

#include "coding/http_zlib_stream.h"

// The one RFC 7692 whole-message deflate and inflate loop, shared by
// per-connection codecs and the shared no-context-takeover codec set. Context
// takeover is the owner's decision: these functions neither reset nor keep the
// state between messages.

namespace ruvia::detail {

// zlib cannot emit an 8-bit raw window, so an 8-bit sender window uses zlib's
// 9-bit window restricted to Huffman coding, which never refers back at all.
[[nodiscard]] constexpr int websocket_deflate_zlib_window_bits(int window_bits) noexcept {
    return -std::max(window_bits, 9);
}

[[nodiscard]] constexpr int websocket_deflate_zlib_strategy(int window_bits) noexcept {
    return window_bits == 8 ? Z_HUFFMAN_ONLY : Z_DEFAULT_STRATEGY;
}

// Compresses a whole message, appending the raw-DEFLATE block to `out` with
// the trailing 0x00 0x00 0xFF 0xFF flush marker removed (RFC 7692 §7.2.1).
[[nodiscard]] inline bool websocket_deflate_message(
    zlib_deflate_stream& deflater, std::string_view input, std::pmr::string& out) {
    auto& stream = deflater.native();
    const auto start = out.size();
    stream.avail_in = 0;
    std::size_t supplied = 0;
    char buffer[4096];
    for (;;) {
        zlib_refill_input(stream, input, supplied);
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = sizeof(buffer);
        const int status = deflate(&stream, supplied == input.size() ? Z_SYNC_FLUSH : Z_NO_FLUSH);
        if (status != Z_OK && status != Z_BUF_ERROR) {
            deflater.rethrow_allocation_failure();
            return false;
        }
        out.append(buffer, sizeof(buffer) - stream.avail_out);
        if (stream.avail_out != 0 && stream.avail_in == 0 && supplied == input.size()) {
            break;
        }
    }
    if (out.size() - start >= 4) {
        out.resize(out.size() - 4);
    }
    if (out.size() == start) {
        out.push_back('\0');
    }
    return true;
}

// One inflate pass over `data`; the message limit covers everything appended
// to `out` since `start`.
[[nodiscard]] inline websocket_inflate_result websocket_inflate_chunk(zlib_inflate_stream& inflater, const char* data,
    std::size_t size, std::pmr::string& out, std::size_t start, protocol_byte_limit message_limit) {
    auto& stream = inflater.native();
    stream.avail_in = 0;
    std::size_t supplied = 0;
    char buffer[8192];
    for (;;) {
        zlib_refill_input(stream, std::string_view(data, size), supplied);
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = sizeof(buffer);
        const int status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_STREAM_END && status != Z_OK && status != Z_BUF_ERROR) {
            inflater.rethrow_allocation_failure();
            return websocket_inflate_result::error;
        }
        const auto produced = sizeof(buffer) - stream.avail_out;
        if (message_limit.addition_exceeds(out.size() - start, produced)) {
            return websocket_inflate_result::too_large;
        }
        out.append(buffer, produced);
        // RFC 7692 permits BFINAL=1 followed by another DEFLATE stream.
        // Preserve the LZ77 window across streams, including takeover
        // history, while discarding the terminated stream's padding bits.
        if (status == Z_STREAM_END && inflateResetKeep(&stream) != Z_OK) {
            inflater.rethrow_allocation_failure();
            return websocket_inflate_result::error;
        }
        if (stream.avail_out != 0 && stream.avail_in == 0 && supplied == size) {
            break;
        }
    }
    return websocket_inflate_result::ok;
}

// Decompresses a whole message: appends the 0x00 0x00 0xFF 0xFF marker that
// the sender stripped, then raw-inflates into `out`, bounded by one explicit
// message limit across the appended bytes to defuse decompression bombs
// (RFC 7692 §7.2.2).
[[nodiscard]] inline websocket_inflate_result websocket_inflate_message(
    zlib_inflate_stream& inflater, std::string_view input, std::pmr::string& out, protocol_byte_limit message_limit) {
    static constexpr unsigned char flush_marker[4] = {0x00, 0x00, 0xFF, 0xFF};
    const auto start = out.size();
    if (const auto result_value = websocket_inflate_chunk(
            inflater, input.data(), input.size(), out, start, message_limit);
        result_value != websocket_inflate_result::ok) {
        return result_value;
    }
    if (const auto result_value = websocket_inflate_chunk(inflater,
            reinterpret_cast<const char*>(flush_marker), sizeof(flush_marker), out, start, message_limit);
        result_value != websocket_inflate_result::ok) {
        return result_value;
    }
    // zlib reports bit 128 only at a complete DEFLATE block boundary.
    // Consuming every input byte alone does not detect a truncated block.
    return (inflater.native().data_type & 128) != 0 ? websocket_inflate_result::ok : websocket_inflate_result::error;
}

}  // namespace ruvia::detail
