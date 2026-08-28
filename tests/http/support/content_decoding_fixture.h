#pragma once

#include "test_harness.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <brotli/encode.h>
#include <zlib.h>
#include <zstd.h>

#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/HttpContentCodec.h"
#include "ruvia/http/detail/request/RequestBodyDecoding.h"
#include "ruvia/http/detail/request/HttpRequestBodyFailure.h"
#include "ruvia/http/detail/http1/Http1ChunkedBodyDecoder.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/http/detail/coding/HttpTransferCodingDecoder.h"
#include "ruvia/http/detail/http1/Http1RequestBodyPlan.h"

namespace content_decoding_test {

using ruvia::decodeHttpContent;
using ruvia::encodeHttpContent;
using ruvia::Http1RequestBodyPlan;
using ruvia::HttpContentCoding;
using ruvia::HttpContentDecodeError;
using ruvia::HttpContentDecodeFailure;
using ruvia::HttpContentDecodeOptions;
using ruvia::HttpContentDecodeResult;
using ruvia::HttpContentEncodeError;
using ruvia::HttpContentEncodeFailure;
using ruvia::HttpContentEncodeOptions;
using ruvia::HttpContentEncodeResult;
using ruvia::HttpDecodedContent;
using ruvia::HttpEncodedContent;
using ruvia::HttpTransferCoding;
using ruvia::HttpTransferCodings;
using ruvia::HttpUnsupportedExpectationPolicy;
using ruvia::parseHttpContentCoding;
using ruvia::ProtocolByteLimit;
using ruvia::detail::decodeHttpRequestContent;
using ruvia::detail::Http1ChunkedBodyDecoder;
using ruvia::detail::Http1ServerRequestParser;
using ruvia::detail::HttpRequestContentDecodeProtocolFailure;
using ruvia::detail::HttpRequestContentDecodeResult;
using ruvia::detail::HttpRequestContentDecoderFailure;
using ruvia::detail::TransferCodingDecodeNeedInput;
using ruvia::detail::TransferCodingDecodeOutput;
using ruvia::detail::TransferCodingDecodeProtocolFailure;
using ruvia::detail::TransferCodingDecoder;
using ruvia::detail::TransferCodingDecodeResult;
using ruvia::detail::TransferCodingDecoderFailure;

inline constexpr std::size_t kDecodedBodyLimit = 16 * 1024 * 1024;

class RejectLargeAllocationResource final : public std::pmr::memory_resource {
public:
    explicit RejectLargeAllocationResource(std::size_t maximumBlockBytes)
        : maximumBlockBytes_(maximumBlockBytes) {}

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (bytes > maximumBlockBytes_) {
            throw std::bad_alloc();
        }
        return std::pmr::get_default_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::get_default_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::size_t maximumBlockBytes_;
};

static_assert(std::same_as<decltype(std::declval<TransferCodingDecoder&>().decode(std::string_view{}, std::span<char>{})), TransferCodingDecodeResult>);
static_assert(!std::default_initializable<TransferCodingDecodeResult>);

template <typename T>
concept HasAnyRvalueTransferCodingDecodeAccessor = requires(T&& result) { std::move(result).needInput(); } || requires(T&& result) { std::move(result).output(); } || requires(T&& result) { std::move(result).complete(); } || requires(T&& result) { std::move(result).protocolFailure(); } || requires(T&& result) { std::move(result).decoderFailure(); };

static_assert(!HasAnyRvalueTransferCodingDecodeAccessor<TransferCodingDecodeResult>);

template <typename T>
concept HasRawTransferDecodeError = requires(const T& result) { result.error(); };

template <typename T>
concept HasRawRequestContentDecodeError = requires(const T& result) { result.error(); };

static_assert(!HasRawTransferDecodeError<TransferCodingDecodeProtocolFailure>);
static_assert(std::same_as<decltype(std::declval<const TransferCodingDecodeProtocolFailure&>().protocolError()), ruvia::HttpProtocolError>);
static_assert(!HasRawRequestContentDecodeError<HttpRequestContentDecodeProtocolFailure>);
static_assert(std::same_as<decltype(std::declval<const HttpRequestContentDecodeProtocolFailure&>().protocolError()), ruvia::HttpProtocolError>);

inline std::string gzipCompress(std::string_view data) {
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

inline TransferDecodeObservation appendTransferDecoded(TransferCodingDecoder& decoder, std::string_view input, std::pmr::string& output) {
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

inline std::string brotliCompress(std::string_view data) {
    std::size_t bound = BrotliEncoderMaxCompressedSize(data.size());
    if (bound == 0) {
        bound = data.size() + 1024;
    }
    std::string out(bound, '\0');
    std::size_t outSize = bound;
    if (BrotliEncoderCompress(BROTLI_DEFAULT_QUALITY, BROTLI_DEFAULT_WINDOW, BROTLI_DEFAULT_MODE, data.size(), reinterpret_cast<const std::uint8_t*>(data.data()), &outSize, reinterpret_cast<std::uint8_t*>(out.data())) != BROTLI_TRUE) {
        return {};
    }
    out.resize(outSize);
    return out;
}

inline std::string zstdCompress(std::string_view data) {
    const std::size_t bound = ZSTD_compressBound(data.size());
    std::string out(bound, '\0');
    const std::size_t size = ZSTD_compress(out.data(), bound, data.data(), data.size(), 3);
    if (ZSTD_isError(size)) {
        return {};
    }
    out.resize(size);
    return out;
}

inline std::string zstdCompressWithWindow(std::string_view data, int windowLog) {
    auto* context = ZSTD_createCCtx();
    if (context == nullptr) {
        return {};
    }
    struct Guard final {
        ZSTD_CCtx* context;
        ~Guard() {
            ZSTD_freeCCtx(context);
        }
    } guard{context};
    if (ZSTD_isError(ZSTD_CCtx_setParameter(context, ZSTD_c_windowLog, windowLog)) != 0 || ZSTD_isError(ZSTD_CCtx_setParameter(context, ZSTD_c_contentSizeFlag, 0)) != 0) {
        return {};
    }
    std::string output(ZSTD_compressBound(data.size()), '\0');
    const auto size = ZSTD_compress2(context, output.data(), output.size(), data.data(), data.size());
    if (ZSTD_isError(size) != 0) {
        return {};
    }
    output.resize(size);
    return output;
}

inline std::string decoded(HttpContentCoding coding, std::string_view input, std::size_t maxBytes) {
    auto result = decodeHttpContent(coding, input, {.maxDecodedBytes = maxBytes, .resource = std::pmr::get_default_resource()});
    const auto* content = result.decoded();
    if (content == nullptr) {
        throw std::runtime_error("test content decode failed");
    }
    return std::string(content->bytes());
}

inline HttpContentDecodeError decodeError(HttpContentCoding coding, std::string_view input, std::size_t maxBytes = kDecodedBodyLimit) {
    const auto result = decodeHttpContent(coding, input, {.maxDecodedBytes = maxBytes, .resource = std::pmr::get_default_resource()});
    const auto* failure = result.failure();
    if (failure == nullptr) {
        throw std::runtime_error("test content decode unexpectedly succeeded");
    }
    return failure->error();
}

inline std::string chunked(std::string_view body) {
    char size[2 * sizeof(std::size_t)];
    const auto [end, ec] = std::to_chars(size, size + sizeof(size), body.size(), 16);
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
concept HasRequestBodyMode = requires(const T& value) { value.mode(); };

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
concept HasValueSemanticRequestExpectations = requires(const T& value) {
    { value.expectations() } -> std::same_as<ruvia::HttpRequestExpectations>;
} && requires(const T&& value) {
    { std::move(value).expectations() } -> std::same_as<ruvia::HttpRequestExpectations>;
};

template <typename T>
concept HasPublicRequestBodyPlanFactories = requires {
    T::makeWithoutBody();
    T::makeKnownLength(std::size_t{});
    T::makeChunked(HttpTransferCodings{});
};

template <typename T>
concept ExposesRvalueEncodedContent = requires(T&& result) { std::move(result).encoded(); };

template <typename T>
concept ExposesRvalueEncodeFailure = requires(const T&& result) { std::move(result).failure(); };

static_assert(!std::default_initializable<Http1RequestBodyPlan>);
static_assert(!std::constructible_from<Http1RequestBodyPlan, ruvia::HttpRequestExpectations>);
static_assert(!std::default_initializable<ruvia::Http1RequestWithoutBody>);
static_assert(!std::default_initializable<ruvia::Http1KnownLengthRequestBody>);
static_assert(!std::default_initializable<ruvia::Http1ChunkedRequestBody>);
static_assert(!std::constructible_from<ruvia::Http1KnownLengthRequestBody, std::size_t>);
static_assert(!std::constructible_from<ruvia::Http1ChunkedRequestBody, HttpTransferCodings>);
static_assert(!HasPublicRequestBodyPlanFactories<Http1RequestBodyPlan>);
static_assert(!HasRequestBodyMode<Http1RequestBodyPlan>);
static_assert(!HasRequestContentLength<Http1RequestBodyPlan>);
static_assert(!HasRequestTransferCodings<Http1RequestBodyPlan>);
static_assert(HasRequestContentLength<ruvia::Http1KnownLengthRequestBody>);
static_assert(!HasRequestContentLength<ruvia::Http1ChunkedRequestBody>);
static_assert(HasRequestTransferCodings<ruvia::Http1ChunkedRequestBody>);
static_assert(HasValueSemanticRequestExpectations<Http1RequestBodyPlan>);
static_assert(!HasRequestTransferCodings<ruvia::Http1KnownLengthRequestBody>);
static_assert(std::same_as<decltype(std::declval<const Http1RequestBodyPlan&>().withoutBody()), const ruvia::Http1RequestWithoutBody*>);
static_assert(std::same_as<decltype(std::declval<const Http1RequestBodyPlan&>().knownLength()), const ruvia::Http1KnownLengthRequestBody*>);
static_assert(std::same_as<decltype(std::declval<const Http1RequestBodyPlan&>().chunked()), const ruvia::Http1ChunkedRequestBody*>);
static_assert(!std::default_initializable<HttpContentDecodeResult>);
static_assert(!std::copy_constructible<HttpContentDecodeResult>);
static_assert(std::move_constructible<HttpContentDecodeResult>);
static_assert(!std::is_move_assignable_v<HttpContentDecodeResult>);
static_assert(!std::default_initializable<HttpDecodedContent>);
static_assert(!std::default_initializable<HttpContentDecodeFailure>);
static_assert(std::same_as<decltype(std::declval<HttpContentDecodeResult&>().decoded()), HttpDecodedContent*>);
static_assert(std::same_as<decltype(std::declval<const HttpContentDecodeResult&>().failure()), const HttpContentDecodeFailure*>);
static_assert(std::same_as<decltype(std::declval<HttpDecodedContent&&>().takeBytes()), std::pmr::string>);
static_assert(std::same_as<decltype(decodeHttpContent(HttpContentCoding::kGzip, std::string_view{}, HttpContentDecodeOptions{.maxDecodedBytes = 0, .resource = nullptr})), HttpContentDecodeResult>);
static_assert(!std::default_initializable<HttpRequestContentDecodeResult>);
static_assert(!std::copy_constructible<HttpRequestContentDecodeResult>);
static_assert(std::move_constructible<HttpRequestContentDecodeResult>);
static_assert(!std::is_move_assignable_v<HttpRequestContentDecodeResult>);
static_assert(std::same_as<decltype(std::declval<HttpRequestContentDecodeResult&>().decoded()), HttpDecodedContent*>);
static_assert(std::same_as<decltype(std::declval<const HttpRequestContentDecodeResult&>().protocolFailure()), const HttpRequestContentDecodeProtocolFailure*>);
static_assert(std::same_as<decltype(std::declval<const HttpRequestContentDecodeResult&>().decoderFailure()), const HttpRequestContentDecoderFailure*>);
static_assert(std::same_as<decltype(decodeHttpRequestContent(HttpContentCoding::kGzip, std::string_view{}, HttpContentDecodeOptions{.maxDecodedBytes = 0, .resource = nullptr})), HttpRequestContentDecodeResult>);
static_assert(!std::default_initializable<HttpContentEncodeResult>);
static_assert(!std::copy_constructible<HttpContentEncodeResult>);
static_assert(std::move_constructible<HttpContentEncodeResult>);
static_assert(!std::is_move_assignable_v<HttpContentEncodeResult>);
static_assert(!std::default_initializable<HttpEncodedContent>);
static_assert(!std::default_initializable<HttpContentEncodeFailure>);
static_assert(!ExposesRvalueEncodedContent<HttpContentEncodeResult>);
static_assert(!ExposesRvalueEncodeFailure<HttpContentEncodeResult>);
static_assert(std::same_as<decltype(std::declval<HttpContentEncodeResult&>().encoded()), HttpEncodedContent*>);
static_assert(std::same_as<decltype(std::declval<const HttpContentEncodeResult&>().failure()), const HttpContentEncodeFailure*>);
static_assert(std::same_as<decltype(std::declval<HttpEncodedContent&&>().takeBytes()), std::pmr::string>);
static_assert(std::same_as<decltype(encodeHttpContent(HttpContentCoding::kGzip, std::string_view{}, HttpContentEncodeOptions{.maxEncodedBytes = 0, .resource = nullptr})), HttpContentEncodeResult>);

inline std::optional<std::string> zstdRoundTrip(std::string_view plain, std::size_t truncateBy) {
    const std::size_t bound = ZSTD_compressBound(plain.size());
    std::string compressed(bound, '\0');
    const std::size_t written = ZSTD_compress(compressed.data(), compressed.size(), plain.data(), plain.size(), 3);
    if (ZSTD_isError(written) != 0 || written <= truncateBy) {
        return std::nullopt;
    }
    compressed.resize(written - truncateBy);

    const auto result = ruvia::decodeHttpContent(HttpContentCoding::kZstd, compressed, {.maxDecodedBytes = ruvia::kDefaultMaxBufferedBodyBytes, .resource = std::pmr::get_default_resource()});
    const auto* decoded = result.decoded();
    if (decoded == nullptr) {
        return std::nullopt;
    }
    return std::string(decoded->bytes());
}

}  // namespace content_decoding_test

using namespace content_decoding_test;  // NOLINT(google-build-using-namespace)
