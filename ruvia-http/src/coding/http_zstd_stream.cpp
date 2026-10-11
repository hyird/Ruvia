#include "coding/http_zstd_stream.h"

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#undef ZSTD_STATIC_LINKING_ONLY

namespace ruvia::detail {

namespace {

inline constexpr int http_zstd_window_log_max = 23;  // RFC 9659: 8 MiB

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
    if (context_ != nullptr &&
        (ZSTD_isError(ZSTD_CCtx_setParameter(context_, ZSTD_c_compressionLevel, ZSTD_CLEVEL_DEFAULT)) != 0 ||
            ZSTD_isError(ZSTD_CCtx_setParameter(context_, ZSTD_c_windowLog, http_zstd_window_log_max)) != 0)) {
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

}  // namespace ruvia::detail
