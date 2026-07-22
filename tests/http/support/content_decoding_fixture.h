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

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <brotli/encode.h>
#include <zlib.h>
#include <zstd.h>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/http/detail/request/RequestBodyDecoding.h"
#include "ruvia/http/detail/request/HttpRequestBodyFailure.h"
#include "ruvia/http/detail/http1/Http1ChunkedBodyDecoder.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/http/detail/coding/HttpTransferCodingDecoder.h"
#include "ruvia/http/detail/http1/Http1RequestBodyPlan.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Error.h"
#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/web/detail/http/context/ContextServices.h"

namespace content_decoding_test {

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

    void do_deallocate(
        void* pointer,
        std::size_t bytes,
        std::size_t alignment) override {
        std::pmr::get_default_resource()->deallocate(
            pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(
        const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::size_t maximumBlockBytes_;
};

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

inline TransferDecodeObservation appendTransferDecoded(
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

inline std::string brotliCompress(std::string_view data) {
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

inline std::string zstdCompressWithWindow(
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

inline std::string decoded(HttpContentCoding coding, std::string_view input, std::size_t maxBytes) {
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

inline HttpContentDecodeError decodeError(
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
    std::optional<ruvia::HttpStatusCode> errorStatus;
};

inline ruvia::Task<std::string_view> readContextText(ruvia::Context& context) {
    co_return co_await context.req().text();
}

inline ruvia::ScopedOperation<std::string_view> makeExpiredContextTextRead() {
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    auto request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setResource(request, memory.resource());
    ruvia::detail::HttpRequestAccess::setBody(request, "body");
    auto context = ruvia::detail::ContextAccess::make(
        memory,
        request,
        ruvia::detail::ContextServices(nullptr, nullptr, nullptr, 1024));
    return context.req().text();
}

inline ruvia::Task<void> awaitExpiredContextTextRead(
    ruvia::ScopedOperation<std::string_view>& operation,
    bool& rejected) {
    try {
        (void)co_await std::move(operation);
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

inline ContextBodyReadObservation readContextGzipBody(
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
        ruvia::detail::taskAsAwaitable(readContextText(context)),
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

inline std::string chunked(std::string_view body) {
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


inline std::optional<std::string> zstdRoundTrip(std::string_view plain, std::size_t truncateBy) {
    const std::size_t bound = ZSTD_compressBound(plain.size());
    std::string compressed(bound, '\0');
    const std::size_t written = ZSTD_compress(
        compressed.data(), compressed.size(), plain.data(), plain.size(), 3);
    if (ZSTD_isError(written) != 0 || written <= truncateBy) {
        return std::nullopt;
    }
    compressed.resize(written - truncateBy);

    const auto result = ruvia::detail::decodeHttpContent(
        HttpContentCoding::kZstd,
        compressed,
        ruvia::kDefaultMaxBufferedBodyBytes,
        std::pmr::get_default_resource());
    const auto* decoded = result.decoded();
    if (decoded == nullptr) {
        return std::nullopt;
    }
    return std::string(decoded->bytes());
}

}  // namespace content_decoding_test

using namespace content_decoding_test;  // NOLINT(google-build-using-namespace)
