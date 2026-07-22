#include "ruvia/http/detail/HttpContentCodec.h"

#include <cstddef>
#include <utility>

#include <zstd.h>
#include <zstd_errors.h>

#include "ruvia/http/detail/PmrResource.h"

// zstd (RFC 8878) with the mandatory 8 MiB HTTP window limit of RFC 9659.

namespace ruvia::detail {

namespace {

inline constexpr int kHttpZstdWindowLogMax = 23;  // RFC 9659: 8 MiB

}  // namespace

ContentDecodeAttempt decodeZstdContent(
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

ContentEncodeAttempt encodeZstdContent(
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
        output.resize(offset + writable);
        ZSTD_outBuffer out{output.data() + offset, writable, 0};
        const auto result = ZSTD_compressStream2(
            context,
            &out,
            &in,
            ZSTD_e_end);
        const auto produced = out.pos;
        output.resize(offset + produced);
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

}  // namespace ruvia::detail
