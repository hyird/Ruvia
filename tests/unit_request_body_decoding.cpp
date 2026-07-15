#include "test_harness.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <brotli/encode.h>
#include <zlib.h>
#include <zstd.h>

#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/http/detail/RequestBodyDecoding.h"
#include "ruvia/http/detail/HttpRequestBodyFailure.h"
#include "ruvia/http/detail/http1/Http1ChunkedBodyDecoder.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/http/detail/body/HttpTransferCodingDecoder.h"
#include "ruvia/http/detail/http1/Http1RequestBodyPlan.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Error.h"
#include "ruvia/web/detail/http/ContextInternal.h"
#include "ruvia/web/detail/http/ContextServices.h"

namespace {

using ruvia::ProtocolByteLimit;
using ruvia::detail::HttpContentCoding;
using ruvia::detail::HttpContentEncodeError;
using ruvia::detail::HttpContentEncodeFailure;
using ruvia::detail::HttpContentEncodeResult;
using ruvia::detail::HttpContentDecodeError;
using ruvia::detail::HttpContentDecodeFailure;
using ruvia::detail::HttpContentDecodeResult;
using ruvia::detail::HttpDecodedContent;
using ruvia::detail::HttpEncodedContent;
using ruvia::detail::HttpRequestContentDecodeProtocolFailure;
using ruvia::detail::HttpRequestContentDecoderFailure;
using ruvia::detail::HttpRequestContentDecodeResult;
using ruvia::detail::Http1ChunkedBodyDecoder;
using ruvia::detail::Http1RequestBodyPlan;
using ruvia::detail::Http1ServerRequestParser;
using ruvia::detail::HttpUnsupportedExpectationPolicy;
using ruvia::detail::HttpTransferCoding;
using ruvia::detail::HttpTransferCodings;
using ruvia::detail::TransferCodingDecoder;
using ruvia::detail::TransferCodingDecodeProtocolFailure;
using ruvia::detail::TransferCodingDecoderFailure;
using ruvia::detail::TransferCodingDecodeNeedInput;
using ruvia::detail::TransferCodingDecodeOutput;
using ruvia::detail::TransferCodingDecodeResult;
using ruvia::detail::decodeHttpContent;
using ruvia::detail::decodeHttpRequestContent;
using ruvia::detail::encodeHttpContent;
using ruvia::detail::httpContentCodingFromFieldValue;

inline constexpr std::size_t kDecodedBodyLimit = 16 * 1024 * 1024;

static_assert(std::same_as<
    decltype(std::declval<TransferCodingDecoder&>().decode(
        std::string_view{}, std::span<char>{})),
    TransferCodingDecodeResult>);
static_assert(!std::default_initializable<TransferCodingDecodeResult>);

template <typename T>
concept HasAnyRvalueTransferCodingDecodeAccessor =
    requires(T&& result) { std::move(result).needInput(); } ||
    requires(T&& result) { std::move(result).output(); } ||
    requires(T&& result) { std::move(result).complete(); } ||
    requires(T&& result) { std::move(result).protocolFailure(); } ||
    requires(T&& result) { std::move(result).decoderFailure(); };

static_assert(!HasAnyRvalueTransferCodingDecodeAccessor<
    TransferCodingDecodeResult>);

template <typename T>
concept HasRawTransferDecodeError = requires(const T& result) {
    result.error();
};

template <typename T>
concept HasRawRequestContentDecodeError = requires(const T& result) {
    result.error();
};

static_assert(!HasRawTransferDecodeError<TransferCodingDecodeProtocolFailure>);
static_assert(std::same_as<
    decltype(std::declval<const TransferCodingDecodeProtocolFailure&>()
        .protocolError()),
    ruvia::HttpProtocolError>);
static_assert(!HasRawRequestContentDecodeError<
    HttpRequestContentDecodeProtocolFailure>);
static_assert(std::same_as<
    decltype(std::declval<const HttpRequestContentDecodeProtocolFailure&>()
        .protocolError()),
    ruvia::HttpProtocolError>);

std::string gzipCompress(std::string_view data) {
    z_stream stream{};
    if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return {};
    }
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    stream.avail_in = static_cast<uInt>(data.size());
    std::string out;
    char buffer[16384];
    int status = Z_OK;
    do {
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = sizeof(buffer);
        status = deflate(&stream, Z_FINISH);
        out.append(buffer, sizeof(buffer) - stream.avail_out);
    } while (status == Z_OK);
    (void)deflateEnd(&stream);
    return out;
}

struct TransferDecodeObservation final {
    bool failed{false};
    std::optional<ruvia::HttpProtocolError> protocolError;
};

TransferDecodeObservation appendTransferDecoded(
    TransferCodingDecoder& decoder,
    std::string_view input,
    std::pmr::string& output) {
    std::array<char, ruvia::detail::kBodyReadChunkBytes> window{};
    for (;;) {
        const auto result = decoder.decode(input, window);
        input.remove_prefix(std::min(input.size(), result.consumedBytes()));
        if (const auto* decoded = result.output()) {
            output.append(decoded->bytes());
            continue;
        }
        if (const auto* failure = result.protocolFailure()) {
            return {true, failure->protocolError()};
        }
        if (result.decoderFailure() != nullptr) {
            return {true, std::nullopt};
        }
        return {};
    }
}

std::string brotliCompress(std::string_view data) {
    std::size_t bound = BrotliEncoderMaxCompressedSize(data.size());
    if (bound == 0) {
        bound = data.size() + 1024;
    }
    std::string out(bound, '\0');
    std::size_t outSize = bound;
    if (BrotliEncoderCompress(
            BROTLI_DEFAULT_QUALITY, BROTLI_DEFAULT_WINDOW, BROTLI_DEFAULT_MODE,
            data.size(), reinterpret_cast<const std::uint8_t*>(data.data()),
            &outSize, reinterpret_cast<std::uint8_t*>(out.data())) != BROTLI_TRUE) {
        return {};
    }
    out.resize(outSize);
    return out;
}

std::string zstdCompress(std::string_view data) {
    const std::size_t bound = ZSTD_compressBound(data.size());
    std::string out(bound, '\0');
    const std::size_t size = ZSTD_compress(out.data(), bound, data.data(), data.size(), 3);
    if (ZSTD_isError(size)) {
        return {};
    }
    out.resize(size);
    return out;
}

std::string zstdCompressWithWindow(
    std::string_view data,
    int windowLog) {
    auto* context = ZSTD_createCCtx();
    if (context == nullptr) {
        return {};
    }
    struct Guard final {
        ZSTD_CCtx* context;
        ~Guard() { ZSTD_freeCCtx(context); }
    } guard{context};
    if (ZSTD_isError(ZSTD_CCtx_setParameter(
            context,
            ZSTD_c_windowLog,
            windowLog)) != 0 ||
        ZSTD_isError(ZSTD_CCtx_setParameter(
            context,
            ZSTD_c_contentSizeFlag,
            0)) != 0) {
        return {};
    }
    std::string output(ZSTD_compressBound(data.size()), '\0');
    const auto size = ZSTD_compress2(
        context,
        output.data(),
        output.size(),
        data.data(),
        data.size());
    if (ZSTD_isError(size) != 0) {
        return {};
    }
    output.resize(size);
    return output;
}

std::string decoded(HttpContentCoding coding, std::string_view input, std::size_t maxBytes) {
    auto result = decodeHttpContent(
        coding,
        input,
        maxBytes,
        std::pmr::get_default_resource());
    const auto* content = result.decoded();
    if (content == nullptr) {
        throw std::runtime_error("test content decode failed");
    }
    return std::string(content->bytes());
}

HttpContentDecodeError decodeError(
    HttpContentCoding coding,
    std::string_view input,
    std::size_t maxBytes = kDecodedBodyLimit) {
    const auto result = decodeHttpContent(
        coding,
        input,
        maxBytes,
        std::pmr::get_default_resource());
    const auto* failure = result.failure();
    if (failure == nullptr) {
        throw std::runtime_error("test content decode unexpectedly succeeded");
    }
    return failure->error();
}

struct ContextBodyReadObservation final {
    std::string body;
    std::uint16_t errorStatus{0};
};

ContextBodyReadObservation readContextGzipBody(
    std::string_view encoded,
    std::size_t maxDecodedBodyBytes) {
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    auto request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setResource(
        request,
        memory.resource());
    const auto contentEncodingSlot =
        ruvia::detail::HttpRequestAccess::knownHeaderSlot(
            ruvia::detail::RequestKnownHeader::kContentEncoding);
    if (!ruvia::detail::HttpRequestAccess::addHeader(
            request,
            ruvia::HttpHeaderView{"Content-Encoding", "gzip"},
            contentEncodingSlot)) {
        throw std::runtime_error(
            "test request rejected Content-Encoding");
    }
    ruvia::detail::HttpRequestAccess::setBody(request, encoded);

    auto context = ruvia::detail::ContextAccess::make(
        memory,
        request,
        ruvia::detail::ContextServices(
            nullptr,
            nullptr,
            nullptr,
            maxDecodedBodyBytes));
    asio::io_context io(1);
    auto future = asio::co_spawn(
        io,
        ruvia::detail::taskAsAwaitable(context.req().text()),
        asio::use_future);
    io.run();

    ContextBodyReadObservation observation;
    try {
        observation.body = future.get();
    } catch (const ruvia::HttpProtocolError& error) {
        observation.errorStatus = error.status();
    }
    return observation;
}

std::string chunked(std::string_view body) {
    char size[2 * sizeof(std::size_t)];
    const auto [end, ec] = std::to_chars(
        size,
        size + sizeof(size),
        body.size(),
        16);
    if (ec != std::errc{}) {
        return {};
    }
    std::string wire(size, end);
    wire.append("\r\n");
    wire.append(body);
    wire.append("\r\n0\r\n\r\n");
    return wire;
}

template <typename T>
concept HasRequestBodyMode = requires(const T& value) {
    value.mode();
};

template <typename T>
concept HasRequestContentLength = requires(const T& value) {
    { value.contentLength() } -> std::same_as<std::size_t>;
};

template <typename T>
concept HasRequestTransferCodings = requires(const T& value) {
    { value.transferCodings() } -> std::same_as<HttpTransferCodings>;
} && requires(const T&& value) {
    { std::move(value).transferCodings() } -> std::same_as<HttpTransferCodings>;
};

template <typename T>
concept HasValueSemanticRequestExpectations =
    requires(const T& value) {
        { value.expectations() } ->
            std::same_as<ruvia::detail::HttpRequestExpectations>;
    } &&
    requires(const T&& value) {
        { std::move(value).expectations() } ->
            std::same_as<ruvia::detail::HttpRequestExpectations>;
    };

template <typename T>
concept HasPublicRequestBodyPlanFactories = requires {
    T::makeWithoutBody();
    T::makeKnownLength(std::size_t{});
    T::makeChunked(HttpTransferCodings{});
};

template <typename T>
concept ExposesRvalueEncodedContent = requires(T&& result) {
    std::move(result).encoded();
};

template <typename T>
concept ExposesRvalueEncodeFailure = requires(const T&& result) {
    std::move(result).failure();
};

static_assert(!std::default_initializable<Http1RequestBodyPlan>);
static_assert(!std::constructible_from<
    Http1RequestBodyPlan,
    ruvia::detail::HttpRequestExpectations>);
static_assert(!std::default_initializable<ruvia::detail::Http1RequestWithoutBody>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1KnownLengthRequestBody>);
static_assert(!std::default_initializable<ruvia::detail::Http1ChunkedRequestBody>);
static_assert(!std::constructible_from<
    ruvia::detail::Http1KnownLengthRequestBody,
    std::size_t>);
static_assert(!std::constructible_from<
    ruvia::detail::Http1ChunkedRequestBody,
    HttpTransferCodings>);
static_assert(!HasPublicRequestBodyPlanFactories<Http1RequestBodyPlan>);
static_assert(!HasRequestBodyMode<Http1RequestBodyPlan>);
static_assert(!HasRequestContentLength<Http1RequestBodyPlan>);
static_assert(!HasRequestTransferCodings<Http1RequestBodyPlan>);
static_assert(HasRequestContentLength<ruvia::detail::Http1KnownLengthRequestBody>);
static_assert(!HasRequestContentLength<ruvia::detail::Http1ChunkedRequestBody>);
static_assert(HasRequestTransferCodings<ruvia::detail::Http1ChunkedRequestBody>);
static_assert(HasValueSemanticRequestExpectations<Http1RequestBodyPlan>);
static_assert(!HasRequestTransferCodings<
    ruvia::detail::Http1KnownLengthRequestBody>);
static_assert(std::same_as<
    decltype(std::declval<const Http1RequestBodyPlan&>().withoutBody()),
    const ruvia::detail::Http1RequestWithoutBody*>);
static_assert(std::same_as<
    decltype(std::declval<const Http1RequestBodyPlan&>().knownLength()),
    const ruvia::detail::Http1KnownLengthRequestBody*>);
static_assert(std::same_as<
    decltype(std::declval<const Http1RequestBodyPlan&>().chunked()),
    const ruvia::detail::Http1ChunkedRequestBody*>);
static_assert(!std::default_initializable<HttpContentDecodeResult>);
static_assert(!std::copy_constructible<HttpContentDecodeResult>);
static_assert(std::move_constructible<HttpContentDecodeResult>);
static_assert(!std::is_move_assignable_v<HttpContentDecodeResult>);
static_assert(!std::default_initializable<HttpDecodedContent>);
static_assert(!std::default_initializable<HttpContentDecodeFailure>);
static_assert(std::same_as<
    decltype(std::declval<HttpContentDecodeResult&>().decoded()),
    HttpDecodedContent*>);
static_assert(std::same_as<
    decltype(std::declval<const HttpContentDecodeResult&>().failure()),
    const HttpContentDecodeFailure*>);
static_assert(std::same_as<
    decltype(std::declval<HttpDecodedContent&&>().takeBytes()),
    std::pmr::string>);
static_assert(std::same_as<
    decltype(decodeHttpContent(
        HttpContentCoding::kGzip,
        std::string_view{},
        std::size_t{},
        static_cast<std::pmr::memory_resource*>(nullptr))),
    HttpContentDecodeResult>);
static_assert(!std::default_initializable<HttpRequestContentDecodeResult>);
static_assert(!std::copy_constructible<HttpRequestContentDecodeResult>);
static_assert(std::move_constructible<HttpRequestContentDecodeResult>);
static_assert(!std::is_move_assignable_v<HttpRequestContentDecodeResult>);
static_assert(std::same_as<
    decltype(std::declval<HttpRequestContentDecodeResult&>().decoded()),
    HttpDecodedContent*>);
static_assert(std::same_as<
    decltype(std::declval<const HttpRequestContentDecodeResult&>()
        .protocolFailure()),
    const HttpRequestContentDecodeProtocolFailure*>);
static_assert(std::same_as<
    decltype(std::declval<const HttpRequestContentDecodeResult&>()
        .decoderFailure()),
    const HttpRequestContentDecoderFailure*>);
static_assert(std::same_as<
    decltype(decodeHttpRequestContent(
        HttpContentCoding::kGzip,
        std::string_view{},
        std::size_t{},
        static_cast<std::pmr::memory_resource*>(nullptr))),
    HttpRequestContentDecodeResult>);
static_assert(!std::default_initializable<HttpContentEncodeResult>);
static_assert(!std::copy_constructible<HttpContentEncodeResult>);
static_assert(std::move_constructible<HttpContentEncodeResult>);
static_assert(!std::is_move_assignable_v<HttpContentEncodeResult>);
static_assert(!std::default_initializable<HttpEncodedContent>);
static_assert(!std::default_initializable<HttpContentEncodeFailure>);
static_assert(!ExposesRvalueEncodedContent<HttpContentEncodeResult>);
static_assert(!ExposesRvalueEncodeFailure<HttpContentEncodeResult>);
static_assert(std::same_as<
    decltype(std::declval<HttpContentEncodeResult&>().encoded()),
    HttpEncodedContent*>);
static_assert(std::same_as<
    decltype(std::declval<const HttpContentEncodeResult&>().failure()),
    const HttpContentEncodeFailure*>);
static_assert(std::same_as<
    decltype(std::declval<HttpEncodedContent&&>().takeBytes()),
    std::pmr::string>);
static_assert(std::same_as<
    decltype(encodeHttpContent(
        HttpContentCoding::kGzip,
        std::string_view{},
        std::size_t{},
        static_cast<std::pmr::memory_resource*>(nullptr))),
    HttpContentEncodeResult>);

}  // namespace

RUVIA_TEST(request_body_failures_own_cross_runtime_http_errors) {
    const auto tooLarge = ruvia::detail::httpRequestBodySizeFailure(
        5, ProtocolByteLimit::limited(4));
    RUVIA_CHECK(tooLarge.has_value());
    if (tooLarge) {
        const auto error = tooLarge->protocolError();
        RUVIA_CHECK_EQ(error.status(), 413);
        RUVIA_CHECK_EQ(
            std::string_view(error.what()),
            std::string_view("request body is too large"));
    }
    RUVIA_CHECK(!ruvia::detail::httpRequestBodyAdditionFailure(
        2, 2, ProtocolByteLimit::limited(4)));
    RUVIA_CHECK(ruvia::detail::httpRequestBodyAdditionFailure(
        2, 3, ProtocolByteLimit::limited(4)).has_value());

    const auto incomplete =
        ruvia::detail::HttpRequestBodyFailure::incomplete().protocolError();
    RUVIA_CHECK_EQ(incomplete.status(), 400);
    RUVIA_CHECK_EQ(
        std::string_view(incomplete.what()),
        std::string_view("incomplete request body"));
}

RUVIA_TEST(http1_request_body_plan_has_one_framing_truth) {
    Http1ServerRequestParser parser;
    const auto noneState = parser.parseMessage(
        "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    const auto& none = noneState.bodyPlan;
    RUVIA_CHECK(none.withoutBody() != nullptr);
    RUVIA_CHECK(none.knownLength() == nullptr);
    RUVIA_CHECK(none.chunked() == nullptr);
    RUVIA_CHECK(!none.requiresConsumption());

    const auto emptyLengthState = parser.parseMessage(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Expect: 100-continue\r\nContent-Length: 0\r\n\r\n");
    const auto& emptyLength = emptyLengthState.bodyPlan;
    const auto* knownLength = emptyLength.knownLength();
    RUVIA_CHECK(knownLength != nullptr);
    RUVIA_CHECK(emptyLength.withoutBody() == nullptr);
    RUVIA_CHECK(emptyLength.chunked() == nullptr);
    if (knownLength != nullptr) {
        RUVIA_CHECK_EQ(knownLength->contentLength(), std::size_t{0});
    }
    RUVIA_CHECK(!emptyLength.requiresConsumption());
    RUVIA_CHECK(emptyLength.expectations().has100Continue());
    const auto emptyExpectationPlan = emptyLength.expectationPlan(
        HttpUnsupportedExpectationPolicy::kReject);
    RUVIA_CHECK(emptyExpectationPlan.noAction() != nullptr);

    const auto compressedChunkedState = parser.parseMessage(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Expect: 100-continue\r\n"
        "Transfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n");
    const auto& compressedChunked = compressedChunkedState.bodyPlan;
    const auto* chunkedBody = compressedChunked.chunked();
    RUVIA_CHECK(chunkedBody != nullptr);
    RUVIA_CHECK(compressedChunked.withoutBody() == nullptr);
    RUVIA_CHECK(compressedChunked.knownLength() == nullptr);
    RUVIA_CHECK(compressedChunked.requiresConsumption());
    const auto compressedExpectationPlan = compressedChunked.expectationPlan(
        HttpUnsupportedExpectationPolicy::kReject);
    RUVIA_CHECK(compressedExpectationPlan.send100Continue() != nullptr);
    if (chunkedBody != nullptr) {
        RUVIA_CHECK_EQ(
            chunkedBody->transferCodings().count,
            std::size_t{1});
    }
}

RUVIA_TEST(http_content_coding_field_mapping_is_protocol_generic) {
    const auto checkCoding = [&](std::string_view value, HttpContentCoding expected) {
        const auto parsed = httpContentCodingFromFieldValue(value);
        RUVIA_CHECK(parsed.unsupported() == nullptr);
        RUVIA_CHECK(parsed.coding() != nullptr);
        if (parsed.coding() != nullptr) {
            RUVIA_CHECK(*parsed.coding() == expected);
        }
    };
    checkCoding("gzip", HttpContentCoding::kGzip);
    checkCoding("x-gzip", HttpContentCoding::kGzip);
    checkCoding("GZIP", HttpContentCoding::kGzip);
    checkCoding("  br ", HttpContentCoding::kBrotli);
    checkCoding("zstd", HttpContentCoding::kZstd);
    checkCoding("identity", HttpContentCoding::kIdentity);
    checkCoding("", HttpContentCoding::kIdentity);

    const auto unsupported = httpContentCodingFromFieldValue("deflate");
    const auto stacked = httpContentCodingFromFieldValue("gzip, br");
    RUVIA_CHECK(unsupported.unsupported() != nullptr);
    RUVIA_CHECK(stacked.unsupported() != nullptr);
}

RUVIA_TEST(request_body_gzip_round_trip) {
    const std::string plain = "The quick brown fox jumps over the lazy dog";
    const std::string gz = gzipCompress(plain);
    RUVIA_CHECK(!gz.empty());
    RUVIA_CHECK_EQ(decoded(HttpContentCoding::kGzip, gz, kDecodedBodyLimit), plain);
}

RUVIA_TEST(request_body_gzip_bomb_rejected) {
    const std::string big(1u << 20, 'a');  // 1 MiB, compresses to a tiny gzip
    const std::string gz = gzipCompress(big);
    // A small cap must stop the expansion, not decode the whole megabyte.
    RUVIA_CHECK(
        decodeError(HttpContentCoding::kGzip, gz, 1024) ==
        HttpContentDecodeError::kDecodedSizeExceeded);
}

RUVIA_TEST(request_body_gzip_truncated_rejected) {
    const std::string plain(4096, 'q');
    std::string gz = gzipCompress(plain);
    RUVIA_CHECK(gz.size() > 6);
    gz.resize(gz.size() - 6);  // cut into the gzip trailer -> incomplete stream
    RUVIA_CHECK(
        decodeError(HttpContentCoding::kGzip, gz) ==
        HttpContentDecodeError::kInvalidContent);
}

RUVIA_TEST(request_body_gzip_decodes_every_rfc1952_member) {
    const std::string first = gzipCompress("first-");
    const std::string second = gzipCompress("second");
    RUVIA_CHECK(!first.empty());
    RUVIA_CHECK(!second.empty());
    RUVIA_CHECK_EQ(
        decoded(HttpContentCoding::kGzip, first + second, kDecodedBodyLimit),
        std::string("first-second"));
}

RUVIA_TEST(request_body_gzip_rejects_bytes_after_the_last_member) {
    std::string encoded = gzipCompress("complete");
    encoded.append("not-a-gzip-member");
    RUVIA_CHECK(
        decodeError(HttpContentCoding::kGzip, encoded) ==
        HttpContentDecodeError::kInvalidContent);
}

RUVIA_TEST(request_body_brotli_round_trip) {
    const std::string plain = "permessage brotli body content, repeated repeated repeated";
    const std::string br = brotliCompress(plain);
    RUVIA_CHECK(!br.empty());
    RUVIA_CHECK_EQ(decoded(HttpContentCoding::kBrotli, br, kDecodedBodyLimit), plain);
}

RUVIA_TEST(request_body_brotli_bomb_rejected) {
    const std::string big(1u << 20, 'a');
    const std::string br = brotliCompress(big);
    RUVIA_CHECK(!br.empty());
    RUVIA_CHECK(
        decodeError(HttpContentCoding::kBrotli, br, 1024) ==
        HttpContentDecodeError::kDecodedSizeExceeded);
}

RUVIA_TEST(request_body_brotli_rejects_trailing_bytes) {
    std::string encoded = brotliCompress("complete");
    encoded.append("trailing");
    RUVIA_CHECK(
        decodeError(HttpContentCoding::kBrotli, encoded) ==
        HttpContentDecodeError::kInvalidContent);
}

RUVIA_TEST(request_body_zstd_round_trip) {
    const std::string plain = "zstd request body content, repeated repeated repeated repeated";
    const std::string zz = zstdCompress(plain);
    RUVIA_CHECK(!zz.empty());
    RUVIA_CHECK_EQ(decoded(HttpContentCoding::kZstd, zz, kDecodedBodyLimit), plain);
}

RUVIA_TEST(request_body_zstd_bomb_rejected) {
    const std::string big(1u << 20, 'a');  // 1 MiB, compresses to a tiny zstd frame
    const std::string zz = zstdCompress(big);
    RUVIA_CHECK(!zz.empty());
    // A small cap must stop the expansion mid-stream, not decode the whole megabyte.
    RUVIA_CHECK(
        decodeError(HttpContentCoding::kZstd, zz, 1024) ==
        HttpContentDecodeError::kDecodedSizeExceeded);
}

RUVIA_TEST(request_body_zstd_decodes_every_rfc8878_frame) {
    const std::string first = zstdCompress("first-");
    const std::string second = zstdCompress("second");
    RUVIA_CHECK(!first.empty());
    RUVIA_CHECK(!second.empty());
    RUVIA_CHECK_EQ(
        decoded(HttpContentCoding::kZstd, first + second, kDecodedBodyLimit),
        std::string("first-second"));
}

RUVIA_TEST(request_body_zstd_rejects_bytes_after_the_last_frame) {
    std::string encoded = zstdCompress("complete");
    encoded.append("not-a-zstd-frame");
    RUVIA_CHECK(
        decodeError(HttpContentCoding::kZstd, encoded) ==
        HttpContentDecodeError::kInvalidContent);
}

RUVIA_TEST(http_zstd_content_rejects_window_above_rfc9659_limit) {
    const std::string plain(9 * 1024 * 1024, 'w');
    const std::string encoded = zstdCompressWithWindow(plain, 24);
    RUVIA_CHECK(!encoded.empty());
    RUVIA_CHECK(
        decodeError(
            HttpContentCoding::kZstd,
            encoded,
            plain.size()) ==
        HttpContentDecodeError::kInvalidContent);

    auto conformant = encodeHttpContent(
        HttpContentCoding::kZstd,
        plain,
        plain.size(),
        std::pmr::get_default_resource());
    RUVIA_CHECK(conformant.encoded() != nullptr);
    if (const auto* content = conformant.encoded()) {
        RUVIA_CHECK_EQ(
            decoded(
                HttpContentCoding::kZstd,
                content->bytes(),
                plain.size()),
            plain);
    }
}

RUVIA_TEST(http_content_encode_enforces_exact_cap_without_partial_output) {
    const std::string input(2048, 'e');
    for (const auto coding : {
             HttpContentCoding::kGzip,
             HttpContentCoding::kBrotli,
             HttpContentCoding::kZstd}) {
        const auto full = encodeHttpContent(
            coding,
            input,
            input.size(),
            std::pmr::get_default_resource());
        RUVIA_CHECK(full.encoded() != nullptr);
        if (full.encoded() == nullptr) {
            continue;
        }
        const auto encodedSize = full.encoded()->bytes().size();
        RUVIA_CHECK(encodedSize > 1);

        const auto exact = encodeHttpContent(
            coding,
            input,
            encodedSize,
            std::pmr::get_default_resource());
        RUVIA_CHECK(exact.encoded() != nullptr);
        if (const auto* encoded = exact.encoded()) {
            RUVIA_CHECK_EQ(encoded->bytes().size(), encodedSize);
        }

        const auto tooSmall = encodeHttpContent(
            coding,
            input,
            encodedSize - 1,
            std::pmr::get_default_resource());
        RUVIA_CHECK(tooSmall.encoded() == nullptr);
        RUVIA_CHECK(tooSmall.failure() != nullptr);
        if (const auto* failure = tooSmall.failure()) {
            RUVIA_CHECK(
                failure->error() ==
                HttpContentEncodeError::kEncodedSizeExceeded);
        }
    }

    const auto identity = encodeHttpContent(
        HttpContentCoding::kIdentity,
        "identity",
        8,
        std::pmr::get_default_resource());
    RUVIA_CHECK(identity.encoded() != nullptr);
    RUVIA_CHECK(identity.failure() == nullptr);
    if (const auto* encoded = identity.encoded()) {
        RUVIA_CHECK_EQ(encoded->bytes(), std::string_view("identity"));
    }
    const auto identityTooLarge = encodeHttpContent(
        HttpContentCoding::kIdentity,
        "identity",
        0,
        std::pmr::get_default_resource());
    RUVIA_CHECK(identityTooLarge.encoded() == nullptr);
    RUVIA_CHECK(identityTooLarge.failure() != nullptr);
    if (const auto* failure = identityTooLarge.failure()) {
        RUVIA_CHECK(
            failure->error() ==
            HttpContentEncodeError::kEncodedSizeExceeded);
    }
}

RUVIA_TEST(http_content_decode_rejects_empty_encoded_input) {
    RUVIA_CHECK(
        decodeError(HttpContentCoding::kGzip, {}) ==
        HttpContentDecodeError::kInvalidContent);
    RUVIA_CHECK(
        decodeError(HttpContentCoding::kBrotli, {}) ==
        HttpContentDecodeError::kInvalidContent);
    RUVIA_CHECK(
        decodeError(HttpContentCoding::kZstd, {}) ==
        HttpContentDecodeError::kInvalidContent);
    RUVIA_CHECK_EQ(
        decoded(HttpContentCoding::kIdentity, {}, 0),
        std::string{});
}

RUVIA_TEST(http_content_decode_zero_cap_allows_only_empty_content) {
    const struct {
        HttpContentCoding coding;
        std::string encoded;
    } emptyCases[] = {
        {HttpContentCoding::kGzip, gzipCompress({})},
        {HttpContentCoding::kBrotli, brotliCompress({})},
        {HttpContentCoding::kZstd, zstdCompress({})},
    };
    for (const auto& test : emptyCases) {
        auto result = decodeHttpContent(
            test.coding,
            test.encoded,
            0,
            std::pmr::get_default_resource());
        RUVIA_CHECK(result.decoded() != nullptr);
        if (const auto* content = result.decoded()) {
            RUVIA_CHECK(content->bytes().empty());
        }
    }
    RUVIA_CHECK(
        decodeError(
            HttpContentCoding::kGzip,
            gzipCompress("x"),
            0) ==
        HttpContentDecodeError::kDecodedSizeExceeded);
}

RUVIA_TEST(web_request_decode_uses_the_configured_buffered_body_limit) {
    const std::string plain(2048, 'x');
    const std::string encoded = gzipCompress(plain);
    RUVIA_CHECK(!encoded.empty());
    const auto observation = readContextGzipBody(encoded, 1024);
    RUVIA_CHECK_EQ(observation.errorStatus, std::uint16_t{413});
    RUVIA_CHECK(observation.body.empty());
}

RUVIA_TEST(web_request_decode_accepts_content_at_the_configured_limit) {
    const std::string plain(2048, 'x');
    const std::string encoded = gzipCompress(plain);
    const auto observation = readContextGzipBody(
        encoded,
        plain.size());
    RUVIA_CHECK_EQ(observation.errorStatus, std::uint16_t{0});
    RUVIA_CHECK_EQ(observation.body, plain);
}

RUVIA_TEST(web_request_decode_rejects_empty_encoded_representation) {
    const auto observation = readContextGzipBody({}, 1024);
    RUVIA_CHECK_EQ(observation.errorStatus, std::uint16_t{400});
    RUVIA_CHECK(observation.body.empty());
}

RUVIA_TEST(http_request_content_decoder_owns_protocol_failure_status) {
    auto* resource = std::pmr::get_default_resource();

    const auto invalid = decodeHttpRequestContent(
        HttpContentCoding::kGzip,
        "not-gzip",
        1024,
        resource);
    RUVIA_CHECK(invalid.protocolFailure() != nullptr);
    RUVIA_CHECK(invalid.decoderFailure() == nullptr);
    RUVIA_CHECK_EQ(invalid.protocolFailure()->protocolError().status(), 400);

    const auto oversized = decodeHttpRequestContent(
        HttpContentCoding::kIdentity,
        "too large",
        4,
        resource);
    RUVIA_CHECK(oversized.protocolFailure() != nullptr);
    RUVIA_CHECK(oversized.decoderFailure() == nullptr);
    RUVIA_CHECK_EQ(oversized.protocolFailure()->protocolError().status(), 413);

    const auto unsupported = decodeHttpRequestContent(
        static_cast<HttpContentCoding>(255),
        {},
        1024,
        resource);
    RUVIA_CHECK(unsupported.protocolFailure() != nullptr);
    RUVIA_CHECK(unsupported.decoderFailure() == nullptr);
    RUVIA_CHECK_EQ(unsupported.protocolFailure()->protocolError().status(), 415);
}

RUVIA_TEST(transfer_coding_decoder_gzip_round_trip) {
    auto* resource = std::pmr::get_default_resource();
    TransferCodingDecoder decoder(
        HttpTransferCoding::kGzip,
        resource,
        ProtocolByteLimit::limited(1u << 20));

    const std::string plain = "transfer-encoding gzip body content, repeated repeated repeated";
    const std::string gz = gzipCompress(plain);
    std::pmr::string output(resource);
    RUVIA_CHECK(!appendTransferDecoded(decoder, gz, output).failed);
    const auto finishResult = decoder.finishInput();
    RUVIA_CHECK(finishResult.complete() != nullptr);
    RUVIA_CHECK_EQ(std::string_view(output.data(), output.size()), std::string_view(plain));
}

RUVIA_TEST(transfer_coded_chunked_request_plan_drives_decode_order) {
    const std::string plain =
        "RFC 9112 transfer coding followed by final chunked framing";
    const std::string gz = gzipCompress(plain);
    const std::string wireBody = chunked(gz);
    RUVIA_CHECK(!gz.empty());
    RUVIA_CHECK(!wireBody.empty());

    Http1ServerRequestParser parser;
    const std::string rawRequest =
        std::string(
            "POST / HTTP/1.1\r\nHost: x\r\n"
            "Transfer-Encoding: gzip, chunked\r\n\r\n") +
        wireBody;
    const auto parsed = parser.parseMessage(rawRequest);
    RUVIA_CHECK(parsed.messageReady());
    const auto* chunkedBody = parsed.bodyPlan.chunked();
    RUVIA_CHECK(chunkedBody != nullptr);
    if (chunkedBody == nullptr) {
        return;
    }
    RUVIA_CHECK_EQ(chunkedBody->transferCodings().count, std::size_t{1});

    auto* resource = std::pmr::get_default_resource();
    Http1ChunkedBodyDecoder chunks(ProtocolByteLimit::limited(1u << 20));
    TransferCodingDecoder transfer(
        chunkedBody->transferCodings().values[0],
        resource,
        ProtocolByteLimit::limited(1u << 20));
    std::pmr::string output(resource);
    std::string_view pending(wireBody);
    bool complete = false;
    while (!complete) {
        const auto result = chunks.decode(pending);
        RUVIA_CHECK(result.needMore() == nullptr);
        if (result.needMore() != nullptr) {
            break;
        }
        if (const auto* bodyChunk = result.bodyChunk()) {
            RUVIA_CHECK(!appendTransferDecoded(
                transfer, bodyChunk->bytes(), output).failed);
        } else if (result.complete() != nullptr) {
            complete = true;
        }
        pending.remove_prefix(result.consumedBytes());
    }
    RUVIA_CHECK(complete);
    const auto finishResult = transfer.finishInput();
    RUVIA_CHECK(finishResult.complete() != nullptr);
    RUVIA_CHECK_EQ(
        std::string_view(output.data(), output.size()),
        std::string_view(plain));
}

RUVIA_TEST(transfer_coding_decoder_rejects_bomb) {
    auto* resource = std::pmr::get_default_resource();
    // A 1 MiB body compresses to a tiny gzip; the decoder must abort the
    // expansion once it passes the small cap, not stage the whole megabyte.
    TransferCodingDecoder decoder(
        HttpTransferCoding::kGzip,
        resource,
        ProtocolByteLimit::limited(1024));

    const std::string big(1u << 20, 'a');
    const std::string gz = gzipCompress(big);
    std::pmr::string output(resource);
    const auto error = appendTransferDecoded(decoder, gz, output);
    RUVIA_CHECK(error.failed);
    RUVIA_CHECK(error.protocolError.has_value());
    RUVIA_CHECK_EQ(error.protocolError->status(), 413);
    const auto finish = decoder.finishInput();
    RUVIA_CHECK(finish.protocolFailure() != nullptr);
    if (finish.protocolFailure() != nullptr) {
        RUVIA_CHECK(
            finish.protocolFailure()->protocolError().status() == 413);
    }
}

RUVIA_TEST(transfer_coding_decoder_reports_typed_wire_failures) {
    auto* resource = std::pmr::get_default_resource();
    std::array<char, ruvia::detail::kBodyReadChunkBytes> window{};

    TransferCodingDecoder invalid(
        HttpTransferCoding::kGzip,
        resource,
        ProtocolByteLimit::limited(1024));
    const auto invalidResult = invalid.decode("not-gzip", window);
    RUVIA_CHECK(invalidResult.protocolFailure() != nullptr);
    RUVIA_CHECK(invalidResult.decoderFailure() == nullptr);
    RUVIA_CHECK_EQ(
        invalidResult.protocolFailure()->protocolError().status(), 400);

    std::string truncated = gzipCompress("truncated");
    truncated.resize(truncated.size() - 4);
    TransferCodingDecoder incomplete(
        HttpTransferCoding::kGzip,
        resource,
        ProtocolByteLimit::limited(1024));
    std::pmr::string ignored(resource);
    RUVIA_CHECK(!appendTransferDecoded(
        incomplete, truncated, ignored).failed);
    const auto incompleteFinish = incomplete.finishInput();
    RUVIA_CHECK(incompleteFinish.protocolFailure() != nullptr);
    if (incompleteFinish.protocolFailure() != nullptr) {
        RUVIA_CHECK(
            incompleteFinish.protocolFailure()->protocolError().status() == 400);
    }
    const auto repeatedFinish = incomplete.finishInput();
    RUVIA_CHECK(repeatedFinish.protocolFailure() != nullptr);
    if (repeatedFinish.protocolFailure() != nullptr) {
        RUVIA_CHECK(
            repeatedFinish.protocolFailure()->protocolError().status() == 400);
    }

    TransferCodingDecoder internalFailure(
        HttpTransferCoding::kGzip,
        resource,
        ProtocolByteLimit::limited(1024));
    const auto decoderFailure = internalFailure.decode("input", {});
    RUVIA_CHECK(decoderFailure.protocolFailure() == nullptr);
    RUVIA_CHECK(decoderFailure.decoderFailure() != nullptr);
    const auto repeatedDecoderFailure = internalFailure.finishInput();
    RUVIA_CHECK(repeatedDecoderFailure.protocolFailure() == nullptr);
    RUVIA_CHECK(repeatedDecoderFailure.decoderFailure() != nullptr);

    std::string trailing = gzipCompress("complete");
    trailing.push_back('x');
    TransferCodingDecoder extra(
        HttpTransferCoding::kGzip,
        resource,
        ProtocolByteLimit::limited(1024));
    const auto trailingError = appendTransferDecoded(
        extra, trailing, ignored);
    RUVIA_CHECK(trailingError.failed);
    RUVIA_CHECK(trailingError.protocolError.has_value());
    RUVIA_CHECK_EQ(trailingError.protocolError->status(), 400);
}
