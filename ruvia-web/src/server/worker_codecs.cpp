#include "server/worker_codecs.h"

#include <exception>
#include <stdexcept>

#include "ruvia/core/failure_report.h"
#include "ruvia/core/memory/pmr_object.h"

namespace ruvia::detail {

worker_codecs::worker_codecs() noexcept
    : gzip_(http_content_coding::gzip, process_resource()),
      deflate_(http_content_coding::deflate, process_resource()),
      brotli_(http_content_coding::brotli, process_resource()),
      zstd_(http_content_coding::zstd, process_resource()),
      websocket_deflate_(process_resource()) {}

worker_codecs::~worker_codecs() {
    for (auto* pool : {&gzip_.streaming_, &deflate_.streaming_, &brotli_.streaming_, &zstd_.streaming_}) {
        for (std::size_t i = 0; i < pool->count_; ++i) {
            destroy_stream_encoder(*pool->free_[i]);
        }
        pool->count_ = 0;
    }
}

http_content_encode_context& worker_codecs::encoder(http_content_coding coding) {
    return encoders(coding).buffered_;
}

worker_codecs::coding_encoders_type& worker_codecs::encoders(http_content_coding coding) {
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

stream_encoder_lease worker_codecs::acquire_stream_encoder(http_content_coding coding) {
    auto& pool = encoders(coding).streaming_;
    if (pool.count_ != 0) {
        return stream_encoder_lease(*this, *pool.free_[--pool.count_]);
    }
    return stream_encoder_lease(
        *this, *construct_pmr_object<http_content_encoder>(
                   resolved_pmr_resource_tag{}, process_resource(), coding, process_resource()));
}

void worker_codecs::release_stream_encoder(http_content_encoder& encoder) noexcept {
    auto& pool = encoders(encoder.coding()).streaming_;
    if (pool.count_ < pool.free_.size()) {
        try {
            encoder.reset();
            pool.free_[pool.count_++] = &encoder;
            return;
        } catch (...) {
            // A codec that cannot be reset is not kept; the next stream
            // constructs a fresh one.
            ruvia::report_unhandled_failure("stream encoder reset", std::current_exception());
        }
    }
    destroy_stream_encoder(encoder);
}

void worker_codecs::destroy_stream_encoder(http_content_encoder& encoder) noexcept {
    destroy_pmr_object(resolved_pmr_resource_tag{}, &encoder, process_resource());
}

}  // namespace ruvia::detail
