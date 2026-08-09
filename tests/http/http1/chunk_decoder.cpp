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
using ruvia::detail::Http1ChunkDecodeFailure;

static_assert(std::same_as<decltype(std::declval<const ProtocolByteLimit&>().maximum()), std::optional<std::size_t>>);
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
concept HasTrailers = requires(const T& result) {
    { result.trailers() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasRvalueTrailers = requires(T&& result) { std::move(result).trailers(); };

template <typename T>
concept HasAnyRvalueHttp1ChunkDecodeAccessor = requires(T&& result) { std::move(result).needMore(); } || requires(T&& result) { std::move(result).bodyChunk(); } || requires(T&& result) { std::move(result).complete(); } || requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasRawDecodeError = requires(const T& result) { result.error(); };

static_assert(std::same_as<decltype(std::declval<Http1ChunkedBodyDecoder&>().decode({})), Http1ChunkDecodeResult>);
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
static_assert(!HasTrailers<Http1ChunkDecodeNeedMore>);
static_assert(!HasTrailers<Http1ChunkDecodeBodyChunk>);
static_assert(HasTrailers<Http1ChunkDecodeComplete>);
static_assert(!HasTrailers<Http1ChunkDecodeFailure>);
static_assert(!HasRvalueTrailers<Http1ChunkDecodeComplete>);
static_assert(!HasRawDecodeError<Http1ChunkDecodeFailure>);
static_assert(std::same_as<decltype(std::declval<const Http1ChunkDecodeFailure&>().protocolError()), ruvia::HttpProtocolError>);

}  // namespace

RUVIA_TEST(protocol_byte_limit_has_no_numeric_sentinel) {
    const auto unlimited = ProtocolByteLimit::unlimited();
    RUVIA_CHECK(!unlimited.isLimited());
    RUVIA_CHECK(!unlimited.maximum().has_value());
    RUVIA_CHECK(!unlimited.exceeds((std::numeric_limits<std::size_t>::max)()));
    RUVIA_CHECK(unlimited.additionExceeds((std::numeric_limits<std::size_t>::max)(), 1));

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
    RUVIA_CHECK_EQ(invalidResult.failure()->protocolError().status(), ruvia::http_status::kBadRequest);
    RUVIA_CHECK_EQ(std::string_view(invalidResult.failure()->protocolError().what()), std::string_view("invalid chunked request body"));
    const auto repeatedInvalid = invalid.decode("0\r\n\r\n");
    RUVIA_CHECK(repeatedInvalid.failure() != nullptr);
    RUVIA_CHECK_EQ(repeatedInvalid.failure()->protocolError().status(), ruvia::http_status::kBadRequest);
    RUVIA_CHECK_EQ(repeatedInvalid.consumedBytes(), std::size_t{0});

    Http1ChunkedBodyDecoder singleLimit(ProtocolByteLimit::limited(10));
    const auto singleLimitResult = singleLimit.decode("b\r\n");
    RUVIA_CHECK(singleLimitResult.failure() != nullptr);
    RUVIA_CHECK_EQ(singleLimitResult.failure()->protocolError().status(), ruvia::http_status::kContentTooLarge);

    Http1ChunkedBodyDecoder accumulated(ProtocolByteLimit::limited(10));
    const std::string_view wire = "8\r\n12345678\r\n5\r\nabcde\r\n0\r\n\r\n";
    const auto first = accumulated.decode(wire);
    RUVIA_CHECK(first.bodyChunk() != nullptr);
    const auto second = accumulated.decode(wire.substr(first.consumedBytes()));
    RUVIA_CHECK(second.failure() != nullptr);
    RUVIA_CHECK_EQ(second.failure()->protocolError().status(), ruvia::http_status::kContentTooLarge);
}

RUVIA_TEST(chunked_body_decoder_separates_body_and_framing_budgets) {
    Http1ChunkedBodyDecoder tinyBody(ProtocolByteLimit::limited(1));
    constexpr std::string_view tinyWire = "1\r\nx\r\n0\r\n\r\n";
    const auto body = tinyBody.decode(tinyWire);
    RUVIA_CHECK(body.bodyChunk() != nullptr);
    if (const auto* chunk = body.bodyChunk()) {
        RUVIA_CHECK_EQ(chunk->bytes(), std::string_view("x"));
        const auto terminal = tinyBody.decode(tinyWire.substr(chunk->consumedBytes()));
        RUVIA_CHECK(terminal.complete() != nullptr);
    }

    Http1ChunkedBodyDecoder framingFlood(ProtocolByteLimit::unlimited());
    std::string sizeLine = "1;x=";
    sizeLine.append(ruvia::kMaxHttpHeaderBytes / 2 - sizeLine.size() - 2, 'a');
    sizeLine.append("\r\n");
    const std::string floodWire = sizeLine + "x\r\n" + sizeLine + "y\r\n0\r\n\r\n";

    const auto first = framingFlood.decode(floodWire);
    RUVIA_CHECK(first.bodyChunk() != nullptr);
    if (const auto* chunk = first.bodyChunk()) {
        const auto excessive = framingFlood.decode(std::string_view(floodWire).substr(chunk->consumedBytes()));
        RUVIA_CHECK(excessive.failure() != nullptr);
        if (const auto* failure = excessive.failure()) {
            RUVIA_CHECK_EQ(failure->protocolError().status(), ruvia::http_status::kContentTooLarge);
        }
    }
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
        if (const auto* complete = result.complete()) {
            RUVIA_CHECK_EQ(complete->trailers(), std::string_view("X-Trace: abc"));
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
    RUVIA_CHECK_EQ(badDelimiter.failure()->protocolError().status(), ruvia::http_status::kBadRequest);

    Http1ChunkedBodyDecoder trailer(ProtocolByteLimit::limited(1024));
    const auto badTrailer = trailer.decode("0\r\nContent-Length: 1\r\n\r\n");
    RUVIA_CHECK(badTrailer.failure() != nullptr);
    RUVIA_CHECK_EQ(badTrailer.failure()->protocolError().status(), ruvia::http_status::kBadRequest);
}

RUVIA_TEST(chunked_body_decoder_caps_each_size_line) {
    Http1ChunkedBodyDecoder decoder(ProtocolByteLimit::limited(ruvia::kDefaultMaxBufferedBodyBytes));
    std::string oversized = "1;x=";
    oversized.append(ruvia::kMaxHttpHeaderBytes, 'a');
    oversized.append("\r\n");

    const auto result = decoder.decode(oversized);
    RUVIA_CHECK(result.failure() != nullptr);
    if (const auto* failure = result.failure()) {
        RUVIA_CHECK_EQ(failure->protocolError().status(), ruvia::http_status::kContentTooLarge);
    }

    Http1ChunkedBodyDecoder boundary(ProtocolByteLimit::limited(ruvia::kDefaultMaxBufferedBodyBytes));
    std::string accepted = "1;x=";
    // Reserve two bytes for the data delimiter and five for the terminal chunk.
    accepted.append(ruvia::kMaxHttpHeaderBytes - 7 - accepted.size() - 2, 'a');
    accepted.append("\r\nx\r\n0\r\n\r\n");
    const auto boundaryResult = boundary.decode(accepted);
    RUVIA_CHECK(boundaryResult.bodyChunk() != nullptr);
    if (const auto* body = boundaryResult.bodyChunk()) {
        const auto terminal = boundary.decode(std::string_view(accepted).substr(body->consumedBytes()));
        RUVIA_CHECK(terminal.complete() != nullptr);
    }
}
