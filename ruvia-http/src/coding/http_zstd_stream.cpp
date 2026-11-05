#include "coding/http_zstd_stream.h"

#include <algorithm>
#include <array>

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#undef ZSTD_STATIC_LINKING_ONLY

#include "ruvia/http/http_content_codec.h"

namespace ruvia::detail {

namespace {

// RFC 9659 §3: HTTP zstd windows never exceed 8 MiB. Decoders must accept
// that window, so it is also the decoder's floor.
inline constexpr int http_zstd_window_log_max = 23;

[[noreturn]] void fail_zstd_encoding(const zstd_encoder_stream& encoder, std::size_t result_value) {
    encoder.rethrow_allocation_failure();
    throw http_content_codec_error(ZSTD_getErrorName(result_value));
}

}  // namespace

zstd_decoder_stream::zstd_decoder_stream(std::pmr::memory_resource* resource)
    : allocation_(resource),
      stream_(ZSTD_createDStream_advanced(ZSTD_customMem{&pmr_codec_allocate, &pmr_codec_free, &allocation_})) {
    if (stream_ != nullptr &&
        (ZSTD_isError(ZSTD_initDStream(stream_)) != 0 ||
            ZSTD_isError(ZSTD_DCtx_setParameter(stream_, ZSTD_d_windowLogMax, http_zstd_window_log_max)) != 0)) {
        (void)ZSTD_freeDStream(stream_);
        stream_ = nullptr;
    }
    if (stream_ == nullptr) {
        allocation_.rethrow_allocation_failure();
    }
}

zstd_decoder_stream::~zstd_decoder_stream() {
    if (stream_ != nullptr) {
        (void)ZSTD_freeDStream(stream_);
    }
}

zstd_encoder_stream::zstd_encoder_stream(std::pmr::memory_resource* resource)
    : allocation_(resource),
      context_(ZSTD_createCCtx_advanced(ZSTD_customMem{&pmr_codec_allocate, &pmr_codec_free, &allocation_})) {
    // The level picks its own window (and adapts it to a known source size).
    // RFC 9659's 8 MiB is only a ceiling; pinning it would also fix the
    // streaming input buffer at the window size for every pooled encoder.
    const bool window_exceeds_http_limit =
        ZSTD_getCParams(ZSTD_CLEVEL_DEFAULT, ZSTD_CONTENTSIZE_UNKNOWN, 0).windowLog >
        static_cast<unsigned>(http_zstd_window_log_max);
    if (context_ != nullptr &&
        (ZSTD_isError(ZSTD_CCtx_setParameter(context_, ZSTD_c_compressionLevel, ZSTD_CLEVEL_DEFAULT)) != 0 ||
            (window_exceeds_http_limit &&
                ZSTD_isError(ZSTD_CCtx_setParameter(context_, ZSTD_c_windowLog, http_zstd_window_log_max)) != 0))) {
        (void)ZSTD_freeCCtx(context_);
        context_ = nullptr;
    }
    if (context_ == nullptr) {
        allocation_.rethrow_allocation_failure();
    }
}

zstd_encoder_stream::~zstd_encoder_stream() {
    if (context_ != nullptr) {
        (void)ZSTD_freeCCtx(context_);
    }
}

bool zstd_encoder_stream::reset() noexcept {
    allocation_.clear_allocation_failure();
    return context_ != nullptr && ZSTD_isError(ZSTD_CCtx_reset(context_, ZSTD_reset_session_only)) == 0;
}

bool zstd_encoder_stream::encode(std::string_view input, std::pmr::string& output,
    ZSTD_EndDirective operation, std::size_t max_encoded_bytes) {
    if (output.size() > max_encoded_bytes) {
        return false;
    }
    ZSTD_inBuffer input_buffer{input.data(), input.size(), 0};
    // Uninitialized scratch: only the bytes zstd produced are copied out.
    std::array<char, 16384> buffer;
    for (;;) {
        // At the cap, a one-byte probe distinguishes "already complete" from
        // "more output pending" without letting a byte past the cap through.
        const auto room = max_encoded_bytes - output.size();
        const bool probing = room == 0;
        const auto writable = probing ? std::size_t{1} : (std::min)(buffer.size(), room);
        ZSTD_outBuffer output_buffer{buffer.data(), writable, 0};
        const auto before_input = input_buffer.pos;
        const auto remaining = ZSTD_compressStream2(context_, &output_buffer, &input_buffer, operation);
        if (ZSTD_isError(remaining) != 0) {
            fail_zstd_encoding(*this, remaining);
        }
        if (probing && output_buffer.pos != 0) {
            return false;
        }
        output.append(buffer.data(), output_buffer.pos);
        const bool input_consumed = input_buffer.pos == input_buffer.size;
        if (input_consumed && (operation == ZSTD_e_continue ? output_buffer.pos < writable : remaining == 0)) {
            return true;
        }
        if (probing) {
            return false;
        }
        if (output_buffer.pos == 0 && input_buffer.pos == before_input) {
            throw http_content_codec_error("Zstd content encoder made no progress");
        }
    }
}

}  // namespace ruvia::detail
