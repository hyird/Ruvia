#include "ruvia/http/HttpContentEncoder.h"

#include <brotli/encode.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#undef ZSTD_STATIC_LINKING_ONLY

#include "ruvia/http/detail/util/HttpPmrObject.h"
#include "ruvia/http/detail/util/PmrResource.h"

#include "coding/PmrCodecAllocation.h"
#include "coding/ZlibPmrAllocation.h"

namespace ruvia {
namespace {

void zlib_free(voidpf, voidpf address) noexcept {
    detail::zlibPmrFree(address);
}

}  // namespace

// Construct the owner before initializing any codec so every partial failure
// unwinds through this destructor, including an exception captured at the C ABI.
struct http_content_encoder::impl final {
    impl(HttpContentCoding coding, std::pmr::memory_resource* resource) noexcept
        : coding(coding),
          allocation(resource) {}
    impl(const impl&) = delete;
    impl& operator=(const impl&) = delete;

    ~impl() {
        switch (coding) {
            case HttpContentCoding::kGzip:
            case HttpContentCoding::deflate:
                if (gzip.state != nullptr) {
                    (void)deflateEnd(&gzip);
                }
                break;
            case HttpContentCoding::kBrotli:
                if (brotli != nullptr) {
                    BrotliEncoderDestroyInstance(brotli);
                }
                break;
            case HttpContentCoding::kZstd:
                if (zstd != nullptr) {
                    (void)ZSTD_freeCCtx(zstd);
                }
                break;
            case HttpContentCoding::kIdentity:
                break;
        }
    }

    [[noreturn]] void fail(const char* message) const {
        allocation.rethrow_allocation_failure();
        throw http_content_encoder_error(message);
    }

    void initialize() {
        switch (coding) {
            case HttpContentCoding::kGzip:
            case HttpContentCoding::deflate:
                gzip.zalloc = &detail::zlib_pmr_allocate_with_exception;
                gzip.zfree = &zlib_free;
                gzip.opaque = &allocation;
                if (deflateInit2(&gzip, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                        coding == HttpContentCoding::kGzip ? 15 + 16 : 15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
                    fail("failed to initialize zlib content encoder");
                }
                return;
            case HttpContentCoding::kBrotli:
                brotli = BrotliEncoderCreateInstance(&detail::pmr_codec_allocate_with_exception,
                    &detail::pmrCodecFree, &allocation);
                if (brotli == nullptr || BrotliEncoderSetParameter(brotli, BROTLI_PARAM_QUALITY, 5) != BROTLI_TRUE) {
                    fail("failed to initialize Brotli content encoder");
                }
                return;
            case HttpContentCoding::kZstd:
                zstd = ZSTD_createCCtx_advanced(ZSTD_customMem{
                    &detail::pmr_codec_allocate_with_exception, &detail::pmrCodecFree, &allocation});
                if (zstd == nullptr ||
                    ZSTD_isError(ZSTD_CCtx_setParameter(zstd, ZSTD_c_compressionLevel, ZSTD_CLEVEL_DEFAULT)) != 0 ||
                    ZSTD_isError(ZSTD_CCtx_setParameter(zstd, ZSTD_c_windowLog, 23)) != 0) {
                    fail("failed to initialize Zstd content encoder");
                }
                return;
            case HttpContentCoding::kIdentity:
                return;
        }
        throw std::invalid_argument("unsupported HTTP content coding");
    }

    void encode_zlib(std::string_view input, std::pmr::string& output, int operation) {
        struct clear_borrows final {
            z_stream& stream;
            ~clear_borrows() {
                stream.next_in = nullptr;
                stream.avail_in = 0;
                stream.next_out = nullptr;
                stream.avail_out = 0;
            }
        } clear{gzip};
        std::size_t supplied = 0;
        std::array<char, 16384> buffer;
        for (;;) {
            if (gzip.avail_in == 0 && supplied < input.size()) {
                const auto count = static_cast<uInt>(
                    std::min<std::size_t>(input.size() - supplied, (std::numeric_limits<uInt>::max)()));
                gzip.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data() + supplied));
                gzip.avail_in = count;
                supplied += count;
            }
            const auto before_input = gzip.avail_in;
            gzip.next_out = reinterpret_cast<Bytef*>(buffer.data());
            gzip.avail_out = static_cast<uInt>(buffer.size());
            const auto status = deflate(&gzip, operation);
            if (status != Z_OK && status != Z_STREAM_END) {
                fail("zlib content encoding failed");
            }
            const auto produced = buffer.size() - gzip.avail_out;
            output.append(buffer.data(), produced);
            if (operation == Z_FINISH && status == Z_STREAM_END) {
                return;
            }
            if (operation != Z_FINISH && gzip.avail_in == 0 && supplied == input.size() && gzip.avail_out != 0) {
                return;
            }
            if (produced == 0 && gzip.avail_in == before_input) {
                fail("zlib content encoder made no progress");
            }
        }
    }

    void encode_brotli(std::string_view input, std::pmr::string& output, BrotliEncoderOperation operation) {
        std::size_t available_input = input.size();
        const auto* next_input = reinterpret_cast<const std::uint8_t*>(input.data());
        for (;;) {
            std::size_t available_output = 0;
            const auto before_input = available_input;
            if (BrotliEncoderCompressStream(brotli, operation, &available_input, &next_input,
                    &available_output, nullptr, nullptr) != BROTLI_TRUE) {
                fail("Brotli content encoding failed");
            }
            std::size_t produced = 0;
            const auto* bytes = BrotliEncoderTakeOutput(brotli, &produced);
            // Consume the borrowed block before the next encoder call.
            if (produced != 0) {
                output.append(reinterpret_cast<const char*>(bytes), produced);
            }
            if (operation == BROTLI_OPERATION_FINISH && BrotliEncoderIsFinished(brotli) == BROTLI_TRUE) {
                return;
            }
            if (available_input == 0 && BrotliEncoderHasMoreOutput(brotli) != BROTLI_TRUE) {
                if (operation == BROTLI_OPERATION_FINISH) {
                    fail("Brotli content encoder did not finish");
                }
                return;
            }
            if (produced == 0 && available_input == before_input) {
                fail("Brotli content encoder made no progress");
            }
        }
    }

    void encode_zstd(std::string_view input, std::pmr::string& output, ZSTD_EndDirective operation) {
        ZSTD_inBuffer input_buffer{input.data(), input.size(), 0};
        std::array<char, 16384> buffer;
        for (;;) {
            ZSTD_outBuffer output_buffer{buffer.data(), buffer.size(), 0};
            const auto before_input = input_buffer.pos;
            const auto remaining = ZSTD_compressStream2(zstd, &output_buffer, &input_buffer, operation);
            if (ZSTD_isError(remaining) != 0) {
                fail("Zstd content encoding failed");
            }
            output.append(buffer.data(), output_buffer.pos);
            if (operation == ZSTD_e_end && remaining == 0 && input_buffer.pos == input_buffer.size) {
                return;
            }
            if (operation != ZSTD_e_end && input_buffer.pos == input_buffer.size &&
                output_buffer.pos < buffer.size() && (operation == ZSTD_e_continue || remaining == 0)) {
                return;
            }
            if (output_buffer.pos == 0 && input_buffer.pos == before_input) {
                fail("Zstd content encoder made no progress");
            }
        }
    }

    void encode(std::string_view input, std::pmr::string& output, bool flush, bool finish) {
        switch (coding) {
            case HttpContentCoding::kGzip:
            case HttpContentCoding::deflate:
                encode_zlib(input, output, finish ? Z_FINISH : flush ? Z_SYNC_FLUSH
                                                                     : Z_NO_FLUSH);
                return;
            case HttpContentCoding::kBrotli:
                encode_brotli(input, output, finish ? BROTLI_OPERATION_FINISH : flush ? BROTLI_OPERATION_FLUSH
                                                                                      : BROTLI_OPERATION_PROCESS);
                return;
            case HttpContentCoding::kZstd:
                encode_zstd(input, output, finish ? ZSTD_e_end : flush ? ZSTD_e_flush
                                                                       : ZSTD_e_continue);
                return;
            case HttpContentCoding::kIdentity:
                return;
        }
    }

    HttpContentCoding coding;
    detail::pmr_codec_allocation_context allocation;
    z_stream gzip{};
    BrotliEncoderState* brotli{nullptr};
    ZSTD_CCtx* zstd{nullptr};
};

http_content_encoder::http_content_encoder(HttpContentCoding coding, std::pmr::memory_resource* resource)
    : coding_(coding),
      resource_(detail::httpPmrResourceOrDefault(resource)) {
    if (coding_ != HttpContentCoding::kIdentity) {
        auto state = detail::makeHttpPmrObject<impl>(resource_, coding_, resource_);
        state->initialize();
        impl_ = state.release();
    }
}

http_content_encoder::~http_content_encoder() {
    detail::destroyHttpPmrObject(impl_, resource_);
}

void http_content_encoder::write(std::string_view input, std::pmr::string& output, bool flush) {
    if (phase_ != phase::active) {
        throw std::logic_error("HTTP content encoder is no longer writable");
    }
    try {
        if (impl_ == nullptr) {
            if (!input.empty()) {
                output.append(input.data(), input.size());
            }
        } else if (!input.empty() || flush) {
            impl_->encode(input, output, flush, false);
        }
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
}

void http_content_encoder::finish(std::pmr::string& output) {
    if (phase_ == phase::finished) {
        return;
    }
    if (phase_ == phase::failed) {
        throw std::logic_error("HTTP content encoder has failed");
    }
    try {
        if (impl_ != nullptr) {
            impl_->encode({}, output, false, true);
        }
        phase_ = phase::finished;
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
}

}  // namespace ruvia
