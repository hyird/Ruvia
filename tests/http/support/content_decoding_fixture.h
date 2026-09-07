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

















inline std::string gzipCompress(std::string_view data) {
    z_stream stream{};
    if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) !=
        Z_OK) {
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
    TransferCodingDecoder& decoder, std::string_view input, std::pmr::string& output) {
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
    if (BrotliEncoderCompress(BROTLI_DEFAULT_QUALITY, BROTLI_DEFAULT_WINDOW, BROTLI_DEFAULT_MODE,
            data.size(), reinterpret_cast<const std::uint8_t*>(data.data()), &outSize,
            reinterpret_cast<std::uint8_t*>(out.data())) != BROTLI_TRUE) {
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
    if (ZSTD_isError(ZSTD_CCtx_setParameter(context, ZSTD_c_windowLog, windowLog)) != 0 ||
        ZSTD_isError(ZSTD_CCtx_setParameter(context, ZSTD_c_contentSizeFlag, 0)) != 0) {
        return {};
    }
    std::string output(ZSTD_compressBound(data.size()), '\0');
    const auto size =
        ZSTD_compress2(context, output.data(), output.size(), data.data(), data.size());
    if (ZSTD_isError(size) != 0) {
        return {};
    }
    output.resize(size);
    return output;
}

inline std::string decoded(HttpContentCoding coding, std::string_view input, std::size_t maxBytes) {
    auto result = decodeHttpContent(
        coding, input, {.maxDecodedBytes = maxBytes, .resource = std::pmr::get_default_resource()});
    const auto* content = result.decoded();
    if (content == nullptr) {
        throw std::runtime_error("test content decode failed");
    }
    return std::string(content->bytes());
}

inline HttpContentDecodeError decodeError(
    HttpContentCoding coding, std::string_view input, std::size_t maxBytes = kDecodedBodyLimit) {
    const auto result = decodeHttpContent(
        coding, input, {.maxDecodedBytes = maxBytes, .resource = std::pmr::get_default_resource()});
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

































































inline std::optional<std::string> zstdRoundTrip(std::string_view plain, std::size_t truncateBy) {
    const std::size_t bound = ZSTD_compressBound(plain.size());
    std::string compressed(bound, '\0');
    const std::size_t written =
        ZSTD_compress(compressed.data(), compressed.size(), plain.data(), plain.size(), 3);
    if (ZSTD_isError(written) != 0 || written <= truncateBy) {
        return std::nullopt;
    }
    compressed.resize(written - truncateBy);

    const auto result = ruvia::decodeHttpContent(HttpContentCoding::kZstd, compressed,
        {.maxDecodedBytes = ruvia::kDefaultMaxBufferedBodyBytes,
            .resource = std::pmr::get_default_resource()});
    const auto* decoded = result.decoded();
    if (decoded == nullptr) {
        return std::nullopt;
    }
    return std::string(decoded->bytes());
}

}  // namespace content_decoding_test

using namespace content_decoding_test;  // NOLINT(google-build-using-namespace)
