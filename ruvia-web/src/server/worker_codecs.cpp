#include "server/worker_codecs.h"

#include <stdexcept>

#include "ruvia/core/memory/pmr_object.h"

namespace ruvia::detail {

worker_codecs::worker_codecs(std::pmr::memory_resource* resource) noexcept
    : resource_(pmr_resource_or_default(resource)),
      gzip_(http_content_coding::gzip, resource),
      deflate_(http_content_coding::deflate, resource),
      brotli_(http_content_coding::brotli, resource),
      zstd_(http_content_coding::zstd, resource),
      websocket_deflate_(resource) {}

worker_codecs::~worker_codecs() {
    for (auto* pool : {&stream_gzip_, &stream_deflate_, &stream_brotli_, &stream_zstd_}) {
        for (std::size_t i = 0; i < pool->count_; ++i) {
            destroy_pmr_object(resolved_pmr_resource_tag{}, pool->free_[i], resource_);
        }
        pool->count_ = 0;
    }
}

http_content_encode_context& worker_codecs::encoder(http_content_coding coding) {
    switch (coding) {
        case http_content_coding::gzip:
            return gzip_;
        case http_content_coding::deflate:
            return deflate_;
        case http_content_coding::brotli:
            return brotli_;
        case http_content_coding::zstd:
            return zstd_;
        case http_content_coding::identity:
            break;
    }
    throw std::logic_error("identity content coding has no encoder");
}

worker_codecs::stream_encoder_pool_type& worker_codecs::stream_encoders(http_content_coding coding) {
    switch (coding) {
        case http_content_coding::gzip:
            return stream_gzip_;
        case http_content_coding::deflate:
            return stream_deflate_;
        case http_content_coding::brotli:
            return stream_brotli_;
        case http_content_coding::zstd:
            return stream_zstd_;
        case http_content_coding::identity:
            break;
    }
    throw std::logic_error("identity content coding has no stream encoder");
}

stream_encoder_lease worker_codecs::acquire_stream_encoder(http_content_coding coding) {
    auto& pool = stream_encoders(coding);
    if (pool.count_ != 0) {
        return stream_encoder_lease(*this, *pool.free_[--pool.count_]);
    }
    return stream_encoder_lease(
        *this, *construct_pmr_object<http_content_encoder>(resolved_pmr_resource_tag{}, resource_, coding, resource_));
}

void worker_codecs::release_stream_encoder(http_content_encoder& encoder) noexcept {
    auto& pool = stream_encoders(encoder.coding());
    if (pool.count_ < pool.free_.size()) {
        try {
            encoder.reset();
            pool.free_[pool.count_++] = &encoder;
            return;
        } catch (...) {
            // A codec that cannot be reset is not kept; the next stream
            // constructs a fresh one.
        }
    }
    destroy_pmr_object(resolved_pmr_resource_tag{}, &encoder, resource_);
}

}  // namespace ruvia::detail
