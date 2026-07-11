#include "test_harness.h"

#include <concepts>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/http1/Http1ChunkedBodyDecoder.h"
#include "ruvia/http/HttpProtocolError.h"

namespace {

using ruvia::HttpProtocolError;
using ruvia::detail::Http1ChunkDecodeBodyChunk;
using ruvia::detail::Http1ChunkDecodeComplete;
using ruvia::detail::Http1ChunkDecodeNeedMore;
using ruvia::detail::Http1ChunkDecodeResult;
using ruvia::detail::Http1ChunkDecoder;
using ruvia::detail::Http1ChunkDelimiterStatus;
using ruvia::detail::Http1ChunkedBodyDecoder;

template <typename T>
concept HasLooseHttp1ChunkDecodeFields = requires(T& result) {
    result.kind = 0;
    result.body = std::string_view{};
    result.consumedBytes = std::size_t{};
};

template <typename T>
concept HasChunkBytes = requires(const T& result) {
    { result.bytes() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasConsumedBytes = requires(const T& result) {
    { result.consumedBytes() } -> std::same_as<std::size_t>;
};

static_assert(std::same_as<
    decltype(std::declval<Http1ChunkedBodyDecoder&>().decode({})),
    Http1ChunkDecodeResult>);
static_assert(!std::default_initializable<Http1ChunkDecodeResult>);
static_assert(!HasLooseHttp1ChunkDecodeFields<Http1ChunkDecodeResult>);
static_assert(HasConsumedBytes<Http1ChunkDecodeNeedMore>);
static_assert(HasConsumedBytes<Http1ChunkDecodeBodyChunk>);
static_assert(HasConsumedBytes<Http1ChunkDecodeComplete>);
static_assert(!HasChunkBytes<Http1ChunkDecodeNeedMore>);
static_assert(HasChunkBytes<Http1ChunkDecodeBodyChunk>);
static_assert(!HasChunkBytes<Http1ChunkDecodeComplete>);

bool sizeLineThrows(Http1ChunkDecoder& decoder, std::string_view line) {
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
    Http1ChunkDecoder decoder(1000);
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
    Http1ChunkDecoder decoder(0);  // 0 == unlimited
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
    Http1ChunkDecoder decoder(0);
    RUVIA_CHECK(decoder.checkDelimiter("\r\n") == Http1ChunkDelimiterStatus::kOk);
    RUVIA_CHECK(decoder.checkDelimiter("\r\nmore") == Http1ChunkDelimiterStatus::kOk);
    RUVIA_CHECK(decoder.checkDelimiter("X") == Http1ChunkDelimiterStatus::kNeedMore);   // fewer than 2 bytes
    RUVIA_CHECK(decoder.checkDelimiter("XY") == Http1ChunkDelimiterStatus::kInvalid);   // not CRLF
    RUVIA_CHECK(decoder.checkDelimiter("\nX") == Http1ChunkDelimiterStatus::kInvalid);
}

RUVIA_TEST(chunk_decoder_rejects_invalid_size_line) {
    Http1ChunkDecoder decoder(0);
    RUVIA_CHECK(sizeLineThrows(decoder, "xyz"));
    RUVIA_CHECK(sizeLineThrows(decoder, ""));
}

RUVIA_TEST(chunk_decoder_single_chunk_over_limit_rejected) {
    Http1ChunkDecoder decoder(10);
    RUVIA_CHECK(sizeLineThrows(decoder, "b"));  // 0xb = 11 > 10 -> 413
}

RUVIA_TEST(chunk_decoder_accumulated_body_over_limit_rejected) {
    Http1ChunkDecoder decoder(10);
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
        Http1ChunkDecoder decoder(0);  // unlimited
        std::size_t size = 0;
        RUVIA_CHECK(decoder.parseSizeLine("fffffffffffffff0", size));  // SIZE_MAX - 15
        RUVIA_CHECK(sizeLineThrows(decoder, "20"));  // +0x20 overflows the total -> 413
    }
}

RUVIA_TEST(chunk_decoder_framing_overhead_is_bounded) {
    // With a tiny limit, the accumulated size-line + CRLF framing overhead alone
    // must eventually trip the 413 guard even for zero-length chunks.
    Http1ChunkDecoder decoder(4);
    std::size_t size = 0;
    bool threw = false;
    try {
        for (int i = 0; i < 100; ++i) {
            (void)decoder.parseSizeLine("0", size);  // each consumes framing bytes
        }
    } catch (const HttpProtocolError&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}

RUVIA_TEST(chunked_body_decoder_emits_zero_copy_chunks_and_preserves_pipeline) {
    Http1ChunkedBodyDecoder decoder(1024);
    const std::string_view wire =
        "5\r\nhello\r\n"
        "6;ext=yes\r\n world\r\n"
        "0\r\nX-Trace: abc\r\n\r\nNEXT";

    std::size_t consumed = 0;
    std::string body;
    for (;;) {
        const auto result = decoder.decode(wire.substr(consumed));
        consumed += result.consumedBytes();
        if (const auto* bodyChunk = result.bodyChunk()) {
            body.append(bodyChunk->bytes());
            continue;
        }
        if (result.complete() != nullptr) {
            break;
        }
        RUVIA_CHECK(result.needMore() == nullptr);
        break;
    }

    RUVIA_CHECK_EQ(body, std::string("hello world"));
    RUVIA_CHECK_EQ(wire.substr(consumed), std::string_view("NEXT"));
}

RUVIA_TEST(chunked_body_decoder_handles_single_byte_input_fragmentation) {
    Http1ChunkedBodyDecoder decoder(1024);
    const std::string wire = "3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n";
    std::string pending;
    std::string body;
    bool complete = false;

    for (const char byte : wire) {
        pending.push_back(byte);
        for (;;) {
            const auto result = decoder.decode(pending);
            if (const auto* bodyChunk = result.bodyChunk()) {
                body.append(bodyChunk->bytes());
            }
            if (result.consumedBytes() != 0) {
                pending.erase(0, result.consumedBytes());
            }
            if (result.complete() != nullptr) {
                complete = true;
                break;
            }
            if (result.needMore() != nullptr) {
                break;
            }
        }
    }

    RUVIA_CHECK(complete);
    RUVIA_CHECK_EQ(body, std::string("abcde"));
    RUVIA_CHECK(pending.empty());
}

RUVIA_TEST(chunked_body_decoder_rejects_bad_delimiter_and_trailer) {
    bool badDelimiter = false;
    try {
        Http1ChunkedBodyDecoder decoder(1024);
        (void)decoder.decode("1\r\nxXY");
    } catch (const std::invalid_argument&) {
        badDelimiter = true;
    }
    RUVIA_CHECK(badDelimiter);

    bool badTrailer = false;
    try {
        Http1ChunkedBodyDecoder decoder(1024);
        (void)decoder.decode("0\r\nContent-Length: 1\r\n\r\n");
    } catch (const std::invalid_argument&) {
        badTrailer = true;
    }
    RUVIA_CHECK(badTrailer);
}
