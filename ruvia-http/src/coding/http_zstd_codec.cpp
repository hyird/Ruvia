#include <cstddef>
#include <utility>

#include "coding/http_content_codec.h"

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#undef ZSTD_STATIC_LINKING_ONLY
#include <zstd_errors.h>

#include "ruvia/http/detail/util/pmr_resource.h"

#include "coding/pmr_codec_allocation.h"

// zstd (RFC 8878) with the mandatory 8 MiB HTTP window limit of RFC 9659.

namespace ruvia::detail {

namespace {

inline constexpr int http_zstd_window_log_max = 23;  // RFC 9659: 8 MiB

}  // namespace

http_content_decode_result decode_zstd_content(
    std::string_view input, std::size_t max_decoded_bytes, std::pmr::memory_resource* resource) {
    std::pmr::string output(http_pmr_resource_or_default(resource));
    pmr_codec_allocation_context allocation_context(output.get_allocator().resource());
    auto* stream = ZSTD_createDStream_advanced(
        ZSTD_customMem{&pmr_codec_allocate_with_exception, &pmr_codec_free, &allocation_context});
    if (stream == nullptr) {
        allocation_context.rethrow_allocation_failure();
        return http_content_decode_result_access::failure(http_content_decode_error::decoder_failure);
    }
    struct guard final {
        ZSTD_DStream* stream_;
        ~guard() {
            ZSTD_freeDStream(stream_);
        }
    } guard_value{stream};
    allocation_context.rethrow_allocation_failure();
    const auto initialized = ZSTD_initDStream(stream);
    if (ZSTD_isError(initialized) != 0) {
        allocation_context.rethrow_allocation_failure();
        return http_content_decode_result_access::failure(http_content_decode_error::decoder_failure);
    }
    const auto window_limit =
        ZSTD_DCtx_setParameter(stream, ZSTD_d_windowLogMax, http_zstd_window_log_max);
    if (ZSTD_isError(window_limit) != 0) {
        allocation_context.rethrow_allocation_failure();
        return http_content_decode_result_access::failure(http_content_decode_error::decoder_failure);
    }

    ZSTD_inBuffer in{input.data(), input.size(), 0};
    char buffer[16384];
    for (;;) {
        const auto before_input = in.pos;
        ZSTD_outBuffer out{buffer, sizeof(buffer), 0};
        const auto result_value = ZSTD_decompressStream(stream, &out, &in);
        if (ZSTD_isError(result_value) != 0) {
            allocation_context.rethrow_allocation_failure();
            return http_content_decode_result_access::failure(ZSTD_getErrorCode(result_value) == ZSTD_error_memory_allocation
                                                                  ? http_content_decode_error::decoder_failure
                                                                  : http_content_decode_error::invalid_content);
        }
        if (!append_decoded_bytes(output, buffer, out.pos, max_decoded_bytes)) {
            return http_content_decode_result_access::failure(http_content_decode_error::decoded_size_exceeded);
        }
        if (result_value == 0 && in.pos == in.size) {
            return http_content_decode_result_access::decoded(std::move(output));
        }
        // A zero result with remaining input completed one RFC 8878 frame;
        // ZSTD_decompressStream is ready to consume the next concatenated frame.
        if (out.pos == 0 && in.pos == before_input) {
            return http_content_decode_result_access::failure(http_content_decode_error::invalid_content);
        }
    }
}

http_content_encode_result encode_zstd_content(
    std::string_view input, std::size_t max_encoded_bytes, std::pmr::memory_resource* resource) {
    std::pmr::string output(http_pmr_resource_or_default(resource));
    pmr_codec_allocation_context allocation_context(output.get_allocator().resource());
    auto* context_value = ZSTD_createCCtx_advanced(
        ZSTD_customMem{&pmr_codec_allocate_with_exception, &pmr_codec_free, &allocation_context});
    if (context_value == nullptr) {
        allocation_context.rethrow_allocation_failure();
        return http_content_encode_result_access::failure(http_content_encode_error::encoder_failure);
    }
    struct guard final {
        ZSTD_CCtx* context_;
        ~guard() {
            ZSTD_freeCCtx(context_);
        }
    } guard_value{context_value};
    allocation_context.rethrow_allocation_failure();
    const auto compression_level =
        ZSTD_CCtx_setParameter(context_value, ZSTD_c_compressionLevel, ZSTD_CLEVEL_DEFAULT);
    if (ZSTD_isError(compression_level) != 0) {
        allocation_context.rethrow_allocation_failure();
        return http_content_encode_result_access::failure(http_content_encode_error::encoder_failure);
    }
    const auto window_log = ZSTD_CCtx_setParameter(context_value, ZSTD_c_windowLog, http_zstd_window_log_max);
    if (ZSTD_isError(window_log) != 0) {
        allocation_context.rethrow_allocation_failure();
        return http_content_encode_result_access::failure(http_content_encode_error::encoder_failure);
    }
    ZSTD_inBuffer in{input.data(), input.size(), 0};
    for (;;) {
        if (output.size() == max_encoded_bytes) {
            char probe_value{};
            ZSTD_outBuffer out{&probe_value, 1, 0};
            const auto result_value = ZSTD_compressStream2(context_value, &out, &in, ZSTD_e_end);
            if (ZSTD_isError(result_value) != 0) {
                allocation_context.rethrow_allocation_failure();
                return http_content_encode_result_access::failure(http_content_encode_error::encoder_failure);
            }
            if (result_value == 0 && out.pos == 0 && in.pos == in.size) {
                return http_content_encode_result_access::encoded(std::move(output));
            }
            return http_content_encode_result_access::failure(http_content_encode_error::encoded_size_exceeded);
        }

        const auto offset = output.size();
        const auto writable = std::min<std::size_t>(8192, max_encoded_bytes - offset);
        const auto before_input = in.pos;
        std::size_t result_value = 0;
        output.resize(offset + writable);
        ZSTD_outBuffer out{output.data() + offset, writable, 0};
        result_value = ZSTD_compressStream2(context_value, &out, &in, ZSTD_e_end);
        output.resize(offset + out.pos);
        if (ZSTD_isError(result_value) != 0) {
            allocation_context.rethrow_allocation_failure();
            return http_content_encode_result_access::failure(http_content_encode_error::encoder_failure);
        }
        if (result_value == 0 && in.pos == in.size) {
            return http_content_encode_result_access::encoded(std::move(output));
        }
        if (output.size() == offset && in.pos == before_input) {
            return http_content_encode_result_access::failure(http_content_encode_error::encoder_failure);
        }
    }
}

}  // namespace ruvia::detail
