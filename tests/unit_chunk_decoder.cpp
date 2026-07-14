#include "test_harness.h"

#include <concepts>
#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/detail/http1/Http1ChunkedBodyDecoder.h"

namespace {

using ruvia::ProtocolByteLimit;
using ruvia::detail::Http1ChunkDecodeBodyChunk;
using ruvia::detail::Http1ChunkDecodeComplete;
using ruvia::detail::Http1ChunkDecodeError;
using ruvia::detail::Http1ChunkDecodeFailure;

static_assert(std::same_as<
    decltype(std::declval<const ProtocolByteLimit&>().maximum()),
    std::optional<std::size_t>>);
using ruvia::detail::Http1ChunkDecodeNeedMore;
using ruvia::detail::Http1ChunkDecodeResult;
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

template <typename T>
concept HasAnyRvalueHttp1ChunkDecodeAccessor =
    requires(T&& result) { std::move(result).needMore(); } ||
    requires(T&& result) { std::move(result).bodyChunk(); } ||
    requires(T&& result) { std::move(result).complete(); } ||
    requires(T&& result) { std::move(result).failure(); };

static_assert(std::same_as<
    decltype(std::declval<Http1ChunkedBodyDecoder&>().decode({})),
    Http1ChunkDecodeResult>);
static_assert(!std::default_initializable<Http1ChunkDecodeResult>);
static_assert(!HasAnyRvalueHttp1ChunkDecodeAccessor<Http1ChunkDecodeResult>);
static_assert(!HasLooseHttp1ChunkDecodeFields<Http1ChunkDecodeResult>);
static_assert(HasConsumedBytes<Http1ChunkDecodeNeedMore>);
static_assert(HasConsumedBytes<Http1ChunkDecodeBodyChunk>);
static_assert(HasConsumedBytes<Http1ChunkDecodeComplete>);
static_assert(HasConsumedBytes<Http1ChunkDecodeFailure>);
static_assert(!HasChunkBytes<Http1ChunkDecodeNeedMore>);
static_assert(HasChunkBytes<Http1ChunkDecodeBodyChunk>);
static_assert(!HasChunkBytes<Http1ChunkDecodeComplete>);
static_assert(!HasChunkBytes<Http1ChunkDecodeFailure>);

}  // namespace

RUVIA_TEST(protocol_byte_limit_has_no_numeric_sentinel) {
    const auto unlimited = ProtocolByteLimit::unlimited();
    RUVIA_CHECK(!unlimited.isLimited());
    RUVIA_CHECK(!unlimited.maximum().has_value());
    RUVIA_CHECK(!unlimited.exceeds((std::numeric_limits<std::size_t>::max)()));
    RUVIA_CHECK(unlimited.additionExceeds(
        (std::numeric_limits<std::size_t>::max)(), 1));

    const auto limited = ProtocolByteLimit::limited(8);
    RUVIA_CHECK(limited.isLimited());
    RUVIA_CHECK(limited.maximum().has_value());
    RUVIA_CHECK_EQ(limited.maximum().value(), std::size_t{8});
    RUVIA_CHECK(!limited.exceeds(8));
    RUVIA_CHECK(limited.exceeds(9));
    RUVIA_CHECK(!limited.additionExceeds(3, 5));
    RUVIA_CHECK(limited.additionExceeds(3, 6));

    bool rejectedZero = false;
    try {
        (void)ProtocolByteLimit::limited(0);
    } catch (const std::invalid_argument&) {
        rejectedZero = true;
    }
    RUVIA_CHECK(rejectedZero);
}

RUVIA_TEST(chunked_body_decoder_reports_typed_size_and_limit_failures) {
    Http1ChunkedBodyDecoder invalid(ProtocolByteLimit::unlimited());
    const auto invalidResult = invalid.decode("xyz\r\n");
    RUVIA_CHECK(invalidResult.failure() != nullptr);
    RUVIA_CHECK(invalidResult.failure()->error() ==
        Http1ChunkDecodeError::kInvalidFraming);
    const auto repeatedInvalid = invalid.decode("0\r\n\r\n");
    RUVIA_CHECK(repeatedInvalid.failure() != nullptr);
    RUVIA_CHECK(repeatedInvalid.failure()->error() ==
        Http1ChunkDecodeError::kInvalidFraming);
    RUVIA_CHECK_EQ(repeatedInvalid.consumedBytes(), std::size_t{0});

    Http1ChunkedBodyDecoder singleLimit(ProtocolByteLimit::limited(10));
    const auto singleLimitResult = singleLimit.decode("b\r\n");
    RUVIA_CHECK(singleLimitResult.failure() != nullptr);
    RUVIA_CHECK(singleLimitResult.failure()->error() ==
        Http1ChunkDecodeError::kBodyTooLarge);

    Http1ChunkedBodyDecoder accumulated(ProtocolByteLimit::limited(10));
    const std::string_view wire = "8\r\n12345678\r\n5\r\nabcde\r\n0\r\n\r\n";
    const auto first = accumulated.decode(wire);
    RUVIA_CHECK(first.bodyChunk() != nullptr);
    const auto second = accumulated.decode(wire.substr(first.consumedBytes()));
    RUVIA_CHECK(second.failure() != nullptr);
    RUVIA_CHECK(second.failure()->error() ==
        Http1ChunkDecodeError::kBodyTooLarge);

    Http1ChunkedBodyDecoder framing(ProtocolByteLimit::limited(4));
    const auto framingResult = framing.decode("0\r\n\r\n");
    RUVIA_CHECK(framingResult.failure() != nullptr);
    RUVIA_CHECK(framingResult.failure()->error() ==
        Http1ChunkDecodeError::kFramingTooLarge);
}

RUVIA_TEST(chunked_body_decoder_emits_zero_copy_chunks_and_preserves_pipeline) {
    Http1ChunkedBodyDecoder decoder(ProtocolByteLimit::limited(1024));
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
    Http1ChunkedBodyDecoder decoder(ProtocolByteLimit::limited(1024));
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
    Http1ChunkedBodyDecoder delimiter(ProtocolByteLimit::limited(1024));
    const auto badDelimiter = delimiter.decode("1\r\nxXY");
    RUVIA_CHECK(badDelimiter.failure() != nullptr);
    RUVIA_CHECK(badDelimiter.failure()->error() ==
        Http1ChunkDecodeError::kInvalidFraming);

    Http1ChunkedBodyDecoder trailer(ProtocolByteLimit::limited(1024));
    const auto badTrailer = trailer.decode(
        "0\r\nContent-Length: 1\r\n\r\n");
    RUVIA_CHECK(badTrailer.failure() != nullptr);
    RUVIA_CHECK(badTrailer.failure()->error() ==
        Http1ChunkDecodeError::kInvalidFraming);
}
