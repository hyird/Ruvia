#include "ruvia/http/detail/HttpContentCoding.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <utility>
#include <variant>

#include <brotli/decode.h>
#include <brotli/encode.h>
#include <zlib.h>
#include <zstd.h>
#include <zstd_errors.h>

#include "ruvia/http/detail/HeaderTokenUtils.h"
#include "ruvia/http/detail/PmrResource.h"

namespace ruvia::detail {
namespace {

using ContentEncodeAttempt =
    std::variant<std::pmr::string, HttpContentEncodeError>;
using ContentDecodeAttempt =
    std::variant<std::pmr::string, HttpContentDecodeError>;

inline constexpr int kHttpZstdWindowLogMax = 23;  // RFC 9659: 8 MiB

[[nodiscard]] bool appendDecodedBytes(
    std::pmr::string& output,
    const char* bytes,
    std::size_t size,
    std::size_t maxDecodedBytes) {
    if (output.size() > maxDecodedBytes ||
        size > maxDecodedBytes - output.size()) {
        return false;
    }
    output.append(bytes, size);
    return true;
}

[[nodiscard]] ContentDecodeAttempt decodeGzipContent(
    std::string_view input,
    std::size_t maxDecodedBytes,
    std::pmr::memory_resource* resource) {
    std::pmr::string output(httpPmrResourceOrDefault(resource));
    z_stream stream{};
    if (inflateInit2(&stream, 15 + 16) != Z_OK) {
        return HttpContentDecodeError::kDecoderFailure;
    }
    struct Guard final {
        z_stream* stream;
        ~Guard() { (void)inflateEnd(stream); }
    } guard{&stream};

    std::size_t supplied = 0;
    const auto refill = [&]() noexcept {
        if (stream.avail_in != 0 || supplied == input.size()) {
            return;
        }
        const auto count = static_cast<uInt>(std::min<std::size_t>(
            input.size() - supplied,
            (std::numeric_limits<uInt>::max)()));
        stream.next_in = reinterpret_cast<Bytef*>(
            const_cast<char*>(input.data() + supplied));
        stream.avail_in = count;
        supplied += count;
    };

    char buffer[16384];
    for (;;) {
        refill();
        const auto beforeInput = stream.avail_in;
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = static_cast<uInt>(sizeof(buffer));
        const int status = inflate(&stream, Z_NO_FLUSH);
        const auto produced = sizeof(buffer) - stream.avail_out;
        if (!appendDecodedBytes(
                output,
                buffer,
                produced,
                maxDecodedBytes)) {
            return HttpContentDecodeError::kDecodedSizeExceeded;
        }

        if (status == Z_STREAM_END) {
            // RFC 1952 gzip data is a series of members. Preserve any input
            // already supplied to zlib, reset only the member state, and keep
            // decoding until the exact HTTP content boundary is consumed.
            refill();
            if (stream.avail_in == 0 && supplied == input.size()) {
                return output;
            }
            auto* nextInput = stream.next_in;
            const auto availableInput = stream.avail_in;
            const int reset = inflateReset2(&stream, 15 + 16);
            if (reset != Z_OK) {
                return HttpContentDecodeError::kDecoderFailure;
            }
            stream.next_in = nextInput;
            stream.avail_in = availableInput;
            continue;
        }
        if (status == Z_MEM_ERROR) {
            return HttpContentDecodeError::kDecoderFailure;
        }
        if (status != Z_OK && status != Z_BUF_ERROR) {
            return HttpContentDecodeError::kInvalidContent;
        }

        const bool progressed =
            produced != 0 || stream.avail_in != beforeInput;
        if (!progressed) {
            if (stream.avail_in == 0 && supplied < input.size()) {
                continue;
            }
            return HttpContentDecodeError::kInvalidContent;
        }
    }
}

[[nodiscard]] bool brotliAllocationFailure(
    BrotliDecoderErrorCode error) noexcept {
    switch (error) {
        case BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MODES:
        case BROTLI_DECODER_ERROR_ALLOC_TREE_GROUPS:
        case BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MAP:
        case BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_1:
        case BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_2:
        case BROTLI_DECODER_ERROR_ALLOC_BLOCK_TYPE_TREES:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] ContentDecodeAttempt decodeBrotliContent(
    std::string_view input,
    std::size_t maxDecodedBytes,
    std::pmr::memory_resource* resource) {
    std::pmr::string output(httpPmrResourceOrDefault(resource));
    auto* state = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    if (state == nullptr) {
        return HttpContentDecodeError::kDecoderFailure;
    }
    struct Guard final {
        BrotliDecoderState* state;
        ~Guard() { BrotliDecoderDestroyInstance(state); }
    } guard{state};

    const auto* nextInput = reinterpret_cast<const std::uint8_t*>(input.data());
    std::size_t availableInput = input.size();
    std::uint8_t buffer[16384];
    for (;;) {
        const auto beforeInput = availableInput;
        auto* nextOutput = buffer;
        std::size_t availableOutput = sizeof(buffer);
        const auto result = BrotliDecoderDecompressStream(
            state,
            &availableInput,
            &nextInput,
            &availableOutput,
            &nextOutput,
            nullptr);
        const auto produced = sizeof(buffer) - availableOutput;
        if (!appendDecodedBytes(
                output,
                reinterpret_cast<const char*>(buffer),
                produced,
                maxDecodedBytes)) {
            return HttpContentDecodeError::kDecodedSizeExceeded;
        }
        if (result == BROTLI_DECODER_RESULT_SUCCESS) {
            // RFC 7932 defines one Brotli stream. Its decoder deliberately does
            // not over-consume, so remaining bytes are not part of this content.
            return availableInput == 0
                ? ContentDecodeAttempt(std::move(output))
                : ContentDecodeAttempt(
                      HttpContentDecodeError::kInvalidContent);
        }
        if (result == BROTLI_DECODER_RESULT_ERROR) {
            return brotliAllocationFailure(
                       BrotliDecoderGetErrorCode(state))
                ? HttpContentDecodeError::kDecoderFailure
                : HttpContentDecodeError::kInvalidContent;
        }
        const bool progressed =
            produced != 0 || availableInput != beforeInput;
        if (!progressed ||
            (result == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT &&
             availableInput == 0)) {
            return HttpContentDecodeError::kInvalidContent;
        }
    }
}

[[nodiscard]] ContentDecodeAttempt decodeZstdContent(
    std::string_view input,
    std::size_t maxDecodedBytes,
    std::pmr::memory_resource* resource) {
    std::pmr::string output(httpPmrResourceOrDefault(resource));
    auto* stream = ZSTD_createDStream();
    if (stream == nullptr) {
        return HttpContentDecodeError::kDecoderFailure;
    }
    struct Guard final {
        ZSTD_DStream* stream;
        ~Guard() { ZSTD_freeDStream(stream); }
    } guard{stream};
    const auto initialized = ZSTD_initDStream(stream);
    if (ZSTD_isError(initialized) != 0) {
        return HttpContentDecodeError::kDecoderFailure;
    }
    const auto windowLimit = ZSTD_DCtx_setParameter(
        stream,
        ZSTD_d_windowLogMax,
        kHttpZstdWindowLogMax);
    if (ZSTD_isError(windowLimit) != 0) {
        return HttpContentDecodeError::kDecoderFailure;
    }

    ZSTD_inBuffer in{input.data(), input.size(), 0};
    char buffer[16384];
    for (;;) {
        const auto beforeInput = in.pos;
        ZSTD_outBuffer out{buffer, sizeof(buffer), 0};
        const auto result = ZSTD_decompressStream(stream, &out, &in);
        if (ZSTD_isError(result) != 0) {
            return ZSTD_getErrorCode(result) ==
                    ZSTD_error_memory_allocation
                ? HttpContentDecodeError::kDecoderFailure
                : HttpContentDecodeError::kInvalidContent;
        }
        if (!appendDecodedBytes(
                output,
                buffer,
                out.pos,
                maxDecodedBytes)) {
            return HttpContentDecodeError::kDecodedSizeExceeded;
        }
        if (result == 0 && in.pos == in.size) {
            return output;
        }
        // A zero result with remaining input completed one RFC 8878 frame;
        // ZSTD_decompressStream is ready to consume the next concatenated frame.
        if (out.pos == 0 && in.pos == beforeInput) {
            return HttpContentDecodeError::kInvalidContent;
        }
    }
}

struct alignas(std::max_align_t) ZlibAllocationHeader final {
    std::pmr::memory_resource* resource;
    std::size_t bytes;
};

voidpf gzipZalloc(voidpf opaque, uInt items, uInt size) noexcept {
    auto* resource = static_cast<std::pmr::memory_resource*>(opaque);
    if (resource == nullptr || items == 0 || size == 0) {
        return nullptr;
    }
    const auto itemBytes = static_cast<std::size_t>(items);
    const auto sizeBytes = static_cast<std::size_t>(size);
    if (itemBytes > (std::numeric_limits<std::size_t>::max)() / sizeBytes) {
        return nullptr;
    }
    const auto payloadBytes = itemBytes * sizeBytes;
    if (payloadBytes > (std::numeric_limits<std::size_t>::max)() - sizeof(ZlibAllocationHeader)) {
        return nullptr;
    }
    const auto totalBytes = sizeof(ZlibAllocationHeader) + payloadBytes;
    try {
        auto* raw = static_cast<std::byte*>(resource->allocate(totalBytes, alignof(ZlibAllocationHeader)));
        auto* header = reinterpret_cast<ZlibAllocationHeader*>(raw);
        header->resource = resource;
        header->bytes = totalBytes;
        return raw + sizeof(ZlibAllocationHeader);
    } catch (...) {
        return nullptr;
    }
}

void gzipZfree(voidpf, voidpf address) noexcept {
    if (address == nullptr) {
        return;
    }
    auto* raw = static_cast<std::byte*>(address) - sizeof(ZlibAllocationHeader);
    auto* header = reinterpret_cast<ZlibAllocationHeader*>(raw);
    header->resource->deallocate(raw, header->bytes, alignof(ZlibAllocationHeader));
}

[[nodiscard]] ContentEncodeAttempt encodeGzipContent(
    std::string_view input,
    std::size_t maxEncodedBytes,
    std::pmr::memory_resource* resource) {
    std::pmr::string output(httpPmrResourceOrDefault(resource));
    z_stream stream{};
    stream.zalloc = &gzipZalloc;
    stream.zfree = &gzipZfree;
    stream.opaque = output.get_allocator().resource();
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return HttpContentEncodeError::kEncoderFailure;
    }
    struct Guard final {
        z_stream* stream;
        ~Guard() { (void)deflateEnd(stream); }
    } guard{&stream};

    std::size_t supplied = 0;
    const auto refill = [&]() noexcept {
        if (stream.avail_in != 0 || supplied == input.size()) {
            return;
        }
        const auto count = static_cast<uInt>(std::min<std::size_t>(
            input.size() - supplied,
            (std::numeric_limits<uInt>::max)()));
        stream.next_in = reinterpret_cast<Bytef*>(
            const_cast<char*>(input.data() + supplied));
        stream.avail_in = count;
        supplied += count;
    };

    for (;;) {
        refill();
        if (output.size() == maxEncodedBytes) {
            if (stream.avail_in == 0 && supplied == input.size()) {
                Bytef probe{};
                stream.next_out = &probe;
                stream.avail_out = 1;
                const auto status = deflate(&stream, Z_FINISH);
                if (status == Z_STREAM_END && stream.avail_out == 1) {
                    return output;
                }
                if (status == Z_MEM_ERROR) {
                    return HttpContentEncodeError::kEncoderFailure;
                }
            }
            return HttpContentEncodeError::kEncodedSizeExceeded;
        }
        const auto offset = output.size();
        const auto writable = std::min<std::size_t>(
            8192,
            maxEncodedBytes - offset);
        const auto beforeInput = stream.avail_in;
        int status = Z_OK;
        output.resize_and_overwrite(
            offset + writable,
            [&stream, &status, offset](char* data, std::size_t count) noexcept {
                const auto available = count - offset;
                stream.next_out = reinterpret_cast<Bytef*>(data + offset);
                stream.avail_out = static_cast<uInt>(available);
                status = deflate(
                    &stream,
                    stream.avail_in == 0 ? Z_FINISH : Z_NO_FLUSH);
                return offset + (available - stream.avail_out);
            });
        if (status == Z_STREAM_END) {
            return output;
        }
        if (status == Z_MEM_ERROR) {
            return HttpContentEncodeError::kEncoderFailure;
        }
        if (status != Z_OK ||
            (output.size() == offset && stream.avail_in == beforeInput)) {
            return HttpContentEncodeError::kEncoderFailure;
        }
    }
}

[[nodiscard]] ContentEncodeAttempt encodeBrotliContent(
    std::string_view input,
    std::size_t maxEncodedBytes,
    std::pmr::memory_resource* resource) {
    std::pmr::string output(httpPmrResourceOrDefault(resource));
    const auto bound = BrotliEncoderMaxCompressedSize(input.size());
    bool ok = false;
    output.resize_and_overwrite(
        maxEncodedBytes,
        [&input, &ok](char* data, std::size_t count) noexcept {
            std::size_t encodedSize = count;
            ok = BrotliEncoderCompress(
                     5,
                     BROTLI_DEFAULT_WINDOW,
                     BROTLI_MODE_GENERIC,
                     input.size(),
                     reinterpret_cast<const std::uint8_t*>(input.data()),
                     &encodedSize,
                     reinterpret_cast<std::uint8_t*>(data)) == BROTLI_TRUE;
            return ok ? encodedSize : std::size_t{0};
        });
    if (ok) {
        return output;
    }
    return bound != 0 && bound > maxEncodedBytes
        ? ContentEncodeAttempt(HttpContentEncodeError::kEncodedSizeExceeded)
        : ContentEncodeAttempt(HttpContentEncodeError::kEncoderFailure);
}

[[nodiscard]] ContentEncodeAttempt encodeZstdContent(
    std::string_view input,
    std::size_t maxEncodedBytes,
    std::pmr::memory_resource* resource) {
    std::pmr::string output(httpPmrResourceOrDefault(resource));
    auto* context = ZSTD_createCCtx();
    if (context == nullptr) {
        return HttpContentEncodeError::kEncoderFailure;
    }
    struct Guard final {
        ZSTD_CCtx* context;
        ~Guard() { ZSTD_freeCCtx(context); }
    } guard{context};
    if (ZSTD_isError(ZSTD_CCtx_setParameter(
            context,
            ZSTD_c_compressionLevel,
            ZSTD_CLEVEL_DEFAULT)) != 0 ||
        ZSTD_isError(ZSTD_CCtx_setParameter(
            context,
            ZSTD_c_windowLog,
            kHttpZstdWindowLogMax)) != 0) {
        return HttpContentEncodeError::kEncoderFailure;
    }
    ZSTD_inBuffer in{input.data(), input.size(), 0};
    for (;;) {
        if (output.size() == maxEncodedBytes) {
            char probe{};
            ZSTD_outBuffer out{&probe, 1, 0};
            const auto result = ZSTD_compressStream2(
                context,
                &out,
                &in,
                ZSTD_e_end);
            if (ZSTD_isError(result) != 0) {
                return HttpContentEncodeError::kEncoderFailure;
            }
            if (result == 0 && out.pos == 0 && in.pos == in.size) {
                return output;
            }
            return HttpContentEncodeError::kEncodedSizeExceeded;
        }

        const auto offset = output.size();
        const auto writable = std::min<std::size_t>(
            8192,
            maxEncodedBytes - offset);
        const auto beforeInput = in.pos;
        std::size_t result = 0;
        std::size_t produced = 0;
        output.resize_and_overwrite(
            offset + writable,
            [&in, &produced, &result, context, offset](
                char* data,
                std::size_t count) noexcept {
                ZSTD_outBuffer out{
                    data + offset,
                    count - offset,
                    0};
                result = ZSTD_compressStream2(
                    context,
                    &out,
                    &in,
                    ZSTD_e_end);
                produced = out.pos;
                return offset + out.pos;
            });
        if (ZSTD_isError(result) != 0) {
            return HttpContentEncodeError::kEncoderFailure;
        }
        if (result == 0 && in.pos == in.size) {
            return output;
        }
        if (produced == 0 && in.pos == beforeInput) {
            return HttpContentEncodeError::kEncoderFailure;
        }
    }
}

}  // namespace

std::string_view httpContentCodingToken(HttpContentCoding coding) noexcept {
    switch (coding) {
        case HttpContentCoding::kBrotli:
            return "br";
        case HttpContentCoding::kZstd:
            return "zstd";
        case HttpContentCoding::kGzip:
            return "gzip";
        case HttpContentCoding::kIdentity:
            return {};
    }
    return {};
}

void HttpContentCodingFieldParser::update(std::string_view value) noexcept {
    if (std::get_if<HttpUnsupportedContentCoding>(&state_) != nullptr) {
        return;
    }
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto comma = value.find(',', begin);
        const auto token = httpTrimOws(value.substr(
            begin,
            comma == std::string_view::npos
                ? std::string_view::npos
                : comma - begin));
        if (!token.empty()) {
            auto* supported = std::get_if<Supported>(&state_);
            ++supported->codingCount;
            if (supported->codingCount > 1) {
                state_.template emplace<HttpUnsupportedContentCoding>();
                return;
            } else if (httpAsciiEqualsIgnoreCase(token, "identity")) {
                supported->coding = HttpContentCoding::kIdentity;
            } else if (httpAsciiEqualsIgnoreCase(token, "gzip") ||
                       httpAsciiEqualsIgnoreCase(token, "x-gzip")) {
                supported->coding = HttpContentCoding::kGzip;
            } else if (httpAsciiEqualsIgnoreCase(token, "br")) {
                supported->coding = HttpContentCoding::kBrotli;
            } else if (httpAsciiEqualsIgnoreCase(token, "zstd")) {
                supported->coding = HttpContentCoding::kZstd;
            } else {
                state_.template emplace<HttpUnsupportedContentCoding>();
                return;
            }
        }
        if (comma == std::string_view::npos) {
            break;
        }
        begin = comma + 1;
    }
}

HttpContentCodingFieldResult HttpContentCodingFieldParser::finish() const noexcept {
    if (std::get_if<HttpUnsupportedContentCoding>(&state_) != nullptr) {
        return HttpContentCodingFieldResult(HttpUnsupportedContentCoding{});
    }
    return HttpContentCodingFieldResult(std::get<Supported>(state_).coding);
}

HttpContentCodingFieldResult httpContentCodingFromFieldValue(
    std::string_view value) noexcept {
    HttpContentCodingFieldParser parser;
    parser.update(value);
    return parser.finish();
}

HttpContentDecodeResult decodeHttpContent(
    HttpContentCoding coding,
    std::string_view input,
    std::size_t maxDecodedBytes,
    std::pmr::memory_resource* resource) {
    ContentDecodeAttempt attempt = HttpContentDecodeError::kUnsupportedCoding;
    switch (coding) {
        case HttpContentCoding::kGzip:
            attempt = decodeGzipContent(
                input,
                maxDecodedBytes,
                resource);
            break;
        case HttpContentCoding::kBrotli:
            attempt = decodeBrotliContent(
                input,
                maxDecodedBytes,
                resource);
            break;
        case HttpContentCoding::kZstd:
            attempt = decodeZstdContent(
                input,
                maxDecodedBytes,
                resource);
            break;
        case HttpContentCoding::kIdentity: {
            if (input.size() > maxDecodedBytes) {
                attempt = HttpContentDecodeError::kDecodedSizeExceeded;
            } else {
                attempt = std::pmr::string(
                    input,
                    httpPmrResourceOrDefault(resource));
            }
            break;
        }
    }
    if (auto* decoded = std::get_if<std::pmr::string>(&attempt)) {
        return HttpContentDecodeResult::makeDecoded(std::move(*decoded));
    }
    return HttpContentDecodeResult::makeFailure(
        std::get<HttpContentDecodeError>(attempt));
}

HttpContentEncodeResult encodeHttpContent(
    HttpContentCoding coding,
    std::string_view input,
    std::size_t maxEncodedBytes,
    std::pmr::memory_resource* resource) {
    ContentEncodeAttempt attempt = HttpContentEncodeError::kEncoderFailure;
    switch (coding) {
        case HttpContentCoding::kBrotli:
            attempt = encodeBrotliContent(
                input,
                maxEncodedBytes,
                resource);
            break;
        case HttpContentCoding::kZstd:
            attempt = encodeZstdContent(
                input,
                maxEncodedBytes,
                resource);
            break;
        case HttpContentCoding::kGzip:
            attempt = encodeGzipContent(
                input,
                maxEncodedBytes,
                resource);
            break;
        case HttpContentCoding::kIdentity: {
            if (input.size() > maxEncodedBytes) {
                attempt = HttpContentEncodeError::kEncodedSizeExceeded;
            } else {
                attempt = std::pmr::string(
                    input,
                    httpPmrResourceOrDefault(resource));
            }
            break;
        }
    }
    if (auto* encoded = std::get_if<std::pmr::string>(&attempt)) {
        return HttpContentEncodeResult::makeEncoded(std::move(*encoded));
    }
    return HttpContentEncodeResult::makeFailure(
        std::get<HttpContentEncodeError>(attempt));
}

}  // namespace ruvia::detail
