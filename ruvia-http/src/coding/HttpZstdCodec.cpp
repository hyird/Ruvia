#include <cstddef>
#include <utility>

#include "ruvia/http/detail/coding/HttpContentCodec.h"

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#undef ZSTD_STATIC_LINKING_ONLY
#include <zstd_errors.h>

#include "ruvia/http/detail/coding/PmrCodecAllocation.h"
#include "ruvia/http/detail/util/PmrResource.h"

// zstd (RFC 8878) with the mandatory 8 MiB HTTP window limit of RFC 9659.

namespace ruvia::detail {

namespace {

inline constexpr int kHttpZstdWindowLogMax = 23;  // RFC 9659: 8 MiB

}  // namespace

HttpContentDecodeResult decodeZstdContent(
    std::string_view input, std::size_t maxDecodedBytes, std::pmr::memory_resource* resource) {
    std::pmr::string output(httpPmrResourceOrDefault(resource));
    auto* stream = ZSTD_createDStream_advanced(
        ZSTD_customMem{&pmrCodecAllocate, &pmrCodecFree, output.get_allocator().resource()});
    if (stream == nullptr) {
        return HttpContentDecodeResultAccess::failure(HttpContentDecodeError::kDecoderFailure);
    }
    struct Guard final {
        ZSTD_DStream* stream;
        ~Guard() {
            ZSTD_freeDStream(stream);
        }
    } guard{stream};
    const auto initialized = ZSTD_initDStream(stream);
    if (ZSTD_isError(initialized) != 0) {
        return HttpContentDecodeResultAccess::failure(HttpContentDecodeError::kDecoderFailure);
    }
    const auto windowLimit =
        ZSTD_DCtx_setParameter(stream, ZSTD_d_windowLogMax, kHttpZstdWindowLogMax);
    if (ZSTD_isError(windowLimit) != 0) {
        return HttpContentDecodeResultAccess::failure(HttpContentDecodeError::kDecoderFailure);
    }

    ZSTD_inBuffer in{input.data(), input.size(), 0};
    char buffer[16384];
    for (;;) {
        const auto beforeInput = in.pos;
        ZSTD_outBuffer out{buffer, sizeof(buffer), 0};
        const auto result = ZSTD_decompressStream(stream, &out, &in);
        if (ZSTD_isError(result) != 0) {
            return HttpContentDecodeResultAccess::failure(ZSTD_getErrorCode(result) == ZSTD_error_memory_allocation
                                                              ? HttpContentDecodeError::kDecoderFailure
                                                              : HttpContentDecodeError::kInvalidContent);
        }
        if (!appendDecodedBytes(output, buffer, out.pos, maxDecodedBytes)) {
            return HttpContentDecodeResultAccess::failure(HttpContentDecodeError::kDecodedSizeExceeded);
        }
        if (result == 0 && in.pos == in.size) {
            return HttpContentDecodeResultAccess::decoded(std::move(output));
        }
        // A zero result with remaining input completed one RFC 8878 frame;
        // ZSTD_decompressStream is ready to consume the next concatenated frame.
        if (out.pos == 0 && in.pos == beforeInput) {
            return HttpContentDecodeResultAccess::failure(HttpContentDecodeError::kInvalidContent);
        }
    }
}

HttpContentEncodeResult encodeZstdContent(
    std::string_view input, std::size_t maxEncodedBytes, std::pmr::memory_resource* resource) {
    std::pmr::string output(httpPmrResourceOrDefault(resource));
    auto* context = ZSTD_createCCtx_advanced(
        ZSTD_customMem{&pmrCodecAllocate, &pmrCodecFree, output.get_allocator().resource()});
    if (context == nullptr) {
        return HttpContentEncodeResultAccess::failure(HttpContentEncodeError::kEncoderFailure);
    }
    struct Guard final {
        ZSTD_CCtx* context;
        ~Guard() {
            ZSTD_freeCCtx(context);
        }
    } guard{context};
    if (ZSTD_isError(
            ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, ZSTD_CLEVEL_DEFAULT)) != 0 ||
        ZSTD_isError(ZSTD_CCtx_setParameter(context, ZSTD_c_windowLog, kHttpZstdWindowLogMax)) !=
            0) {
        return HttpContentEncodeResultAccess::failure(HttpContentEncodeError::kEncoderFailure);
    }
    ZSTD_inBuffer in{input.data(), input.size(), 0};
    for (;;) {
        if (output.size() == maxEncodedBytes) {
            char probe{};
            ZSTD_outBuffer out{&probe, 1, 0};
            const auto result = ZSTD_compressStream2(context, &out, &in, ZSTD_e_end);
            if (ZSTD_isError(result) != 0) {
                return HttpContentEncodeResultAccess::failure(HttpContentEncodeError::kEncoderFailure);
            }
            if (result == 0 && out.pos == 0 && in.pos == in.size) {
                return HttpContentEncodeResultAccess::encoded(std::move(output));
            }
            return HttpContentEncodeResultAccess::failure(HttpContentEncodeError::kEncodedSizeExceeded);
        }

        const auto offset = output.size();
        const auto writable = std::min<std::size_t>(8192, maxEncodedBytes - offset);
        const auto beforeInput = in.pos;
        std::size_t result = 0;
        output.resize_and_overwrite(offset + writable, [&](char* bytes, std::size_t) noexcept {
            ZSTD_outBuffer out{bytes + offset, writable, 0};
            result = ZSTD_compressStream2(context, &out, &in, ZSTD_e_end);
            return offset + out.pos;
        });
        if (ZSTD_isError(result) != 0) {
            return HttpContentEncodeResultAccess::failure(HttpContentEncodeError::kEncoderFailure);
        }
        if (result == 0 && in.pos == in.size) {
            return HttpContentEncodeResultAccess::encoded(std::move(output));
        }
        if (output.size() == offset && in.pos == beforeInput) {
            return HttpContentEncodeResultAccess::failure(HttpContentEncodeError::kEncoderFailure);
        }
    }
}

}  // namespace ruvia::detail
