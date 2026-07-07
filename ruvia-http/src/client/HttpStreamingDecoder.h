#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include <brotli/decode.h>
#include <zlib.h>
#include <zstd.h>

#include "../HeaderAcceptUtils.h"  // HttpContentCoding

namespace ruvia::detail {

// Incremental (streaming) decompressor for a single response Content-Encoding. Unlike the
// one-shot decoders in RequestBodyDecoding.h it retains codec state across calls, so a streamed
// body can be decoded slice-by-slice. Each decode() consumes as much of `in` as it can while
// appending at most `maxOut` bytes to `out`, then rewrites `in` to the unconsumed remainder --
// capping per-call output bounds memory under readChunk backpressure and stops a small compressed
// chunk from expanding without limit in one step. Truncation is detected at end of input via
// finished() (the stream must have reached its logical end).
class StreamingContentDecoder final {
public:
    explicit StreamingContentDecoder(HttpContentCoding coding) noexcept : coding_(coding) {
        switch (coding_) {
            case HttpContentCoding::kGzip:
                zlibOk_ = inflateInit2(&zstream_, 15 + 16) == Z_OK;  // gzip framing
                break;
            case HttpContentCoding::kBrotli:
                brotli_ = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
                break;
            case HttpContentCoding::kZstd:
                zstd_ = ZSTD_createDStream();
                break;
            case HttpContentCoding::kNone:
                break;
        }
    }

    ~StreamingContentDecoder() {
        if (zlibOk_) {
            (void)inflateEnd(&zstream_);
        }
        if (brotli_ != nullptr) {
            BrotliDecoderDestroyInstance(brotli_);
        }
        if (zstd_ != nullptr) {
            ZSTD_freeDStream(zstd_);
        }
    }

    StreamingContentDecoder(const StreamingContentDecoder&) = delete;
    StreamingContentDecoder& operator=(const StreamingContentDecoder&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        switch (coding_) {
            case HttpContentCoding::kGzip:
                return zlibOk_;
            case HttpContentCoding::kBrotli:
                return brotli_ != nullptr;
            case HttpContentCoding::kZstd:
                return zstd_ != nullptr;
            case HttpContentCoding::kNone:
                return false;
        }
        return false;
    }

    [[nodiscard]] bool finished() const noexcept { return finished_; }

    // Decode from `in` into `out` (appending at most maxOut bytes), advancing `in` past the
    // consumed prefix. Returns false on a malformed stream.
    [[nodiscard]] bool decode(std::string_view& in, std::pmr::string& out, std::size_t maxOut) {
        if (!valid()) {
            return false;
        }
        if (finished_) {
            in = {};  // trailing bytes after the stream end are discarded
            return true;
        }
        switch (coding_) {
            case HttpContentCoding::kGzip:
                return decodeZlib(in, out, maxOut);
            case HttpContentCoding::kBrotli:
                return decodeBrotli(in, out, maxOut);
            case HttpContentCoding::kZstd:
                return decodeZstd(in, out, maxOut);
            case HttpContentCoding::kNone:
                return false;
        }
        return false;
    }

private:
    bool decodeZlib(std::string_view& in, std::pmr::string& out, std::size_t maxOut) {
        zstream_.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
        zstream_.avail_in = static_cast<uInt>(in.size());
        char buffer[16384];
        std::size_t produced = 0;
        while (produced < maxOut) {
            const uInt beforeIn = zstream_.avail_in;
            const auto room = std::min<std::size_t>(sizeof(buffer), maxOut - produced);
            zstream_.next_out = reinterpret_cast<Bytef*>(buffer);
            zstream_.avail_out = static_cast<uInt>(room);
            const int status = inflate(&zstream_, Z_NO_FLUSH);
            if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR) {
                in = std::string_view(
                    reinterpret_cast<const char*>(zstream_.next_in), zstream_.avail_in);
                return false;
            }
            const auto n = room - zstream_.avail_out;
            out.append(buffer, n);
            produced += n;
            if (status == Z_STREAM_END) {
                finished_ = true;
                break;
            }
            // No forward progress (Z_BUF_ERROR with no input, or a stall) -> need more input.
            if (n == 0 && zstream_.avail_in == beforeIn) {
                break;
            }
        }
        in = std::string_view(reinterpret_cast<const char*>(zstream_.next_in), zstream_.avail_in);
        return true;
    }

    bool decodeBrotli(std::string_view& in, std::pmr::string& out, std::size_t maxOut) {
        const std::uint8_t* nextIn = reinterpret_cast<const std::uint8_t*>(in.data());
        std::size_t availIn = in.size();
        std::uint8_t buffer[16384];
        std::size_t produced = 0;
        while (produced < maxOut) {
            const std::size_t beforeIn = availIn;
            const std::size_t room = std::min<std::size_t>(sizeof(buffer), maxOut - produced);
            std::uint8_t* nextOut = buffer;
            std::size_t availOut = room;
            const BrotliDecoderResult result =
                BrotliDecoderDecompressStream(brotli_, &availIn, &nextIn, &availOut, &nextOut, nullptr);
            if (result == BROTLI_DECODER_RESULT_ERROR) {
                in = std::string_view(reinterpret_cast<const char*>(nextIn), availIn);
                return false;
            }
            const auto n = room - availOut;
            out.append(reinterpret_cast<const char*>(buffer), n);
            produced += n;
            if (result == BROTLI_DECODER_RESULT_SUCCESS) {
                finished_ = true;
                break;
            }
            if (result == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) {
                break;  // consumed all supplied input; wait for more
            }
            // NEEDS_MORE_OUTPUT: loop until maxOut is hit; guard against a no-progress stall.
            if (n == 0 && availIn == beforeIn) {
                break;
            }
        }
        in = std::string_view(reinterpret_cast<const char*>(nextIn), availIn);
        return true;
    }

    bool decodeZstd(std::string_view& in, std::pmr::string& out, std::size_t maxOut) {
        ZSTD_inBuffer input{in.data(), in.size(), 0};
        char buffer[16384];
        std::size_t produced = 0;
        while (produced < maxOut) {
            const std::size_t beforeInPos = input.pos;
            const std::size_t room = std::min<std::size_t>(sizeof(buffer), maxOut - produced);
            ZSTD_outBuffer output{buffer, room, 0};
            const std::size_t result = ZSTD_decompressStream(zstd_, &output, &input);
            if (ZSTD_isError(result) != 0) {
                in = in.substr(input.pos);
                return false;
            }
            out.append(buffer, output.pos);
            produced += output.pos;
            if (result == 0) {
                finished_ = true;  // frame fully decoded and flushed
                break;
            }
            if (output.pos == 0 && input.pos == beforeInPos) {
                break;  // no progress: need more input
            }
        }
        in = in.substr(input.pos);
        return true;
    }

    z_stream zstream_{};
    BrotliDecoderState* brotli_{nullptr};
    ZSTD_DStream* zstd_{nullptr};
    HttpContentCoding coding_;
    bool zlibOk_{false};
    bool finished_{false};
};

}  // namespace ruvia::detail
