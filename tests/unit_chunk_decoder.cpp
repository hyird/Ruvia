#include "test_harness.h"

#include <cstddef>
#include <string_view>

#include "ruvia/http/detail/HttpBodyFramer.h"
#include "ruvia/http/Error.h"

namespace {

using ruvia::HttpError;
using ruvia::detail::ChunkDelimiterStatus;
using ruvia::detail::HttpChunkDecoder;

bool sizeLineThrows(HttpChunkDecoder& decoder, std::string_view line) {
    std::size_t size = 0;
    try {
        (void)decoder.parseSizeLine(line, size);
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

}  // namespace

RUVIA_TEST(chunk_decoder_basic_two_chunk_flow) {
    HttpChunkDecoder decoder(1000);
    std::size_t size = 0;
    RUVIA_CHECK(decoder.parseSizeLine("5", size));  // non-final chunk
    RUVIA_CHECK_EQ(size, std::size_t{5});
    RUVIA_CHECK_EQ(decoder.remaining(), std::size_t{5});
    RUVIA_CHECK(!decoder.awaitingDelimiter());
    decoder.consumeBodyBytes(5);
    RUVIA_CHECK_EQ(decoder.remaining(), std::size_t{0});
    RUVIA_CHECK(decoder.awaitingDelimiter());  // chunk fully read -> expect CRLF next
    decoder.consumeDelimiter();
    RUVIA_CHECK(!decoder.awaitingDelimiter());
    RUVIA_CHECK(!decoder.parseSizeLine("0", size));  // size 0 -> last chunk
}

RUVIA_TEST(chunk_decoder_hex_size_and_partial_consume) {
    HttpChunkDecoder decoder(0);  // 0 == unlimited
    std::size_t size = 0;
    RUVIA_CHECK(decoder.parseSizeLine("1a", size));  // 0x1a
    RUVIA_CHECK_EQ(size, std::size_t{26});
    decoder.consumeBodyBytes(10);
    RUVIA_CHECK_EQ(decoder.remaining(), std::size_t{16});
    RUVIA_CHECK(!decoder.awaitingDelimiter());
    decoder.consumeBodyBytes(16);
    RUVIA_CHECK(decoder.awaitingDelimiter());
}

RUVIA_TEST(chunk_decoder_check_delimiter) {
    HttpChunkDecoder decoder(0);
    RUVIA_CHECK(decoder.checkDelimiter("\r\n") == ChunkDelimiterStatus::kOk);
    RUVIA_CHECK(decoder.checkDelimiter("\r\nmore") == ChunkDelimiterStatus::kOk);
    RUVIA_CHECK(decoder.checkDelimiter("X") == ChunkDelimiterStatus::kNeedMore);   // fewer than 2 bytes
    RUVIA_CHECK(decoder.checkDelimiter("XY") == ChunkDelimiterStatus::kInvalid);   // not CRLF
    RUVIA_CHECK(decoder.checkDelimiter("\nX") == ChunkDelimiterStatus::kInvalid);
}

RUVIA_TEST(chunk_decoder_rejects_invalid_size_line) {
    HttpChunkDecoder decoder(0);
    RUVIA_CHECK(sizeLineThrows(decoder, "xyz"));
    RUVIA_CHECK(sizeLineThrows(decoder, ""));
}

RUVIA_TEST(chunk_decoder_single_chunk_over_limit_rejected) {
    HttpChunkDecoder decoder(10);
    RUVIA_CHECK(sizeLineThrows(decoder, "b"));  // 0xb = 11 > 10 -> 413
}

RUVIA_TEST(chunk_decoder_accumulated_body_over_limit_rejected) {
    HttpChunkDecoder decoder(10);
    std::size_t size = 0;
    RUVIA_CHECK(decoder.parseSizeLine("8", size));  // 8 <= 10
    decoder.consumeBodyBytes(8);
    decoder.consumeDelimiter();
    // A further 5 bytes would push the decoded total to 13 > 10.
    RUVIA_CHECK(sizeLineThrows(decoder, "5"));
}

RUVIA_TEST(chunk_decoder_rejects_decoded_size_integer_overflow) {
    // With no body-size limit (maxBodyBytes_ == 0) the per-chunk and cumulative
    // limit guards are both inert, so the ONLY defense against decodedBytes_ +
    // chunkSize wrapping past SIZE_MAX -- which would silently reset the running
    // total and defeat size accounting -- is the overflow guard. parseSizeLine
    // accepts a chunk size right up to SIZE_MAX, so a near-max chunk followed by
    // a small one must trip 413 rather than wrap. (32-bit size_t cannot express a
    // 64-bit near-max literal, which parseSizeLine rejects earlier, so guard it.)
    if constexpr (sizeof(std::size_t) >= 8) {
        HttpChunkDecoder decoder(0);  // unlimited
        std::size_t size = 0;
        RUVIA_CHECK(decoder.parseSizeLine("fffffffffffffff0", size));  // SIZE_MAX - 15
        RUVIA_CHECK(sizeLineThrows(decoder, "20"));  // +0x20 overflows the total -> 413
    }
}

RUVIA_TEST(chunk_decoder_framing_overhead_is_bounded) {
    // With a tiny limit, the accumulated size-line + CRLF framing overhead alone
    // must eventually trip the 413 guard even for zero-length chunks.
    HttpChunkDecoder decoder(4);
    std::size_t size = 0;
    bool threw = false;
    try {
        for (int i = 0; i < 100; ++i) {
            (void)decoder.parseSizeLine("0", size);  // each consumes framing bytes
        }
    } catch (const HttpError&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}
