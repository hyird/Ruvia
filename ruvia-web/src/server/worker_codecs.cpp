#include "server/worker_codecs.h"

#include <stdexcept>

namespace ruvia::detail {

worker_codecs::worker_codecs(std::pmr::memory_resource* resource) noexcept
    : gzip_(http_content_coding::gzip, resource),
      deflate_(http_content_coding::deflate, resource),
      brotli_(http_content_coding::brotli, resource),
      zstd_(http_content_coding::zstd, resource),
      websocket_deflate_(resource) {}

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

}  // namespace ruvia::detail
