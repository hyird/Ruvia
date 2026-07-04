#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include <brotli/decode.h>
#include <zlib.h>
#include <zstd.h>

#include "HeaderAcceptUtils.h"
#include "HeaderTokenUtils.h"
#include "ruvia/http/HttpRequest.h"

namespace ruvia::detail {

// Upper bound on a decoded request body, independent of the (already bounded)
// compressed size, so a small compressed body cannot expand without limit.
inline constexpr std::size_t kMaxDecodedRequestBodyBytes = 64 * 1024 * 1024;

inline bool zlibInflateRequestBody(std::string_view input, std::pmr::string& out, std::size_t maxBytes, int windowBits) {
    z_stream stream{};
    if (inflateInit2(&stream, windowBits) != Z_OK) {
        return false;
    }
    struct Guard final {
        z_stream* stream;
        ~Guard() { (void)inflateEnd(stream); }
    } guard{&stream};
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    char buffer[16384];
    int status = Z_OK;
    do {
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = sizeof(buffer);
        status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR) {
            return false;
        }
        const auto produced = sizeof(buffer) - stream.avail_out;
        if (maxBytes != 0 && produced > maxBytes - out.size()) {
            return false;
        }
        out.append(buffer, produced);
        if (status == Z_BUF_ERROR) {
            return false;
        }
    } while (status != Z_STREAM_END);
    return true;
}

inline bool brotliInflateRequestBody(std::string_view input, std::pmr::string& out, std::size_t maxBytes) {
    BrotliDecoderState* state = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    if (state == nullptr) {
        return false;
    }
    struct Guard final {
        BrotliDecoderState* state;
        ~Guard() { BrotliDecoderDestroyInstance(state); }
    } guard{state};
    const std::uint8_t* nextIn = reinterpret_cast<const std::uint8_t*>(input.data());
    std::size_t availIn = input.size();
    std::uint8_t buffer[16384];
    BrotliDecoderResult result = BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT;
    while (result != BROTLI_DECODER_RESULT_SUCCESS) {
        std::uint8_t* nextOut = buffer;
        std::size_t availOut = sizeof(buffer);
        result = BrotliDecoderDecompressStream(state, &availIn, &nextIn, &availOut, &nextOut, nullptr);
        if (result == BROTLI_DECODER_RESULT_ERROR) {
            return false;
        }
        const auto produced = sizeof(buffer) - availOut;
        if (maxBytes != 0 && produced > maxBytes - out.size()) {
            return false;
        }
        out.append(reinterpret_cast<const char*>(buffer), produced);
        if (result == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && availIn == 0) {
            return false;  // truncated stream
        }
    }
    return true;
}

inline bool zstdInflateRequestBody(std::string_view input, std::pmr::string& out, std::size_t maxBytes) {
    ZSTD_DStream* stream = ZSTD_createDStream();
    if (stream == nullptr) {
        return false;
    }
    struct Guard final {
        ZSTD_DStream* stream;
        ~Guard() { ZSTD_freeDStream(stream); }
    } guard{stream};
    ZSTD_inBuffer in{input.data(), input.size(), 0};
    char buffer[16384];
    // Mirror the zlib/brotli decoders: a truncated frame must be rejected, not
    // reported as a complete body. ZSTD_decompressStream returns 0 only once a
    // frame is fully decoded and flushed; any other (non-error) value means more
    // decoding/flushing is still pending.
    for (;;) {
        ZSTD_outBuffer outBuffer{buffer, sizeof(buffer), 0};
        const std::size_t result = ZSTD_decompressStream(stream, &outBuffer, &in);
        if (ZSTD_isError(result) != 0) {
            return false;
        }
        if (maxBytes != 0 && outBuffer.pos > maxBytes - out.size()) {
            return false;
        }
        out.append(buffer, outBuffer.pos);
        if (result == 0) {
            // Frame completely decoded and flushed.
            break;
        }
        if (outBuffer.pos == sizeof(buffer)) {
            // Output buffer filled; flush the remainder before requiring input.
            continue;
        }
        if (in.pos >= in.size) {
            // Frame incomplete but no input remains: the stream was truncated.
            return false;
        }
    }
    return true;
}

// Decodes a request body carrying a single Content-Encoding into `out`; returns
// false on a malformed stream or one exceeding maxBytes (decompression bomb).
[[nodiscard]] inline bool decodeRequestContentEncoding(
    HttpContentCoding coding, std::string_view input, std::pmr::string& out, std::size_t maxBytes) {
    switch (coding) {
        case HttpContentCoding::kGzip:
            return zlibInflateRequestBody(input, out, maxBytes, 15 + 16);
        case HttpContentCoding::kBrotli:
            return brotliInflateRequestBody(input, out, maxBytes);
        case HttpContentCoding::kZstd:
            return zstdInflateRequestBody(input, out, maxBytes);
        case HttpContentCoding::kNone:
            break;
    }
    return false;
}

// Maps a request Content-Encoding header to a coding we can decode. Only a single
// coding token is honored; a list (multiple codings) or an unknown/identity token
// yields kNone so the body is delivered as received.
[[nodiscard]] inline HttpContentCoding requestContentCoding(std::string_view contentEncoding) noexcept {
    const auto token = httpTrimOws(contentEncoding);
    if (token.empty() || token.find(',') != std::string_view::npos) {
        return HttpContentCoding::kNone;
    }
    if (httpAsciiEqualsIgnoreCase(token, "gzip") || httpAsciiEqualsIgnoreCase(token, "x-gzip")) {
        return HttpContentCoding::kGzip;
    }
    if (httpAsciiEqualsIgnoreCase(token, "br")) {
        return HttpContentCoding::kBrotli;
    }
    if (httpAsciiEqualsIgnoreCase(token, "zstd")) {
        return HttpContentCoding::kZstd;
    }
    return HttpContentCoding::kNone;
}

[[nodiscard]] inline HttpContentCoding requestContentCoding(const HttpRequest& request) noexcept {
    bool seen = false;
    std::string_view value;
    for (const auto& header : request.headers()) {
        if (!httpAsciiEqualsIgnoreCase(header.name(), "Content-Encoding")) {
            continue;
        }
        if (seen) {
            return HttpContentCoding::kNone;
        }
        seen = true;
        value = header.value();
    }
    return seen ? requestContentCoding(value) : HttpContentCoding::kNone;
}

}  // namespace ruvia::detail
