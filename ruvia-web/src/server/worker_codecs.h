#pragma once

#include <memory_resource>

#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/websocket_deflate_codecs.h"

namespace ruvia::detail {

// The web worker's reusable codec state. Everything is created on first use
// from the worker resource and lives until the worker is destroyed, so codec
// memory is sized per worker rather than per request or connection. Only
// synchronous work on the owning worker thread may borrow it: inline response
// compression takes one encoder per content coding (each coding has one fixed
// level), and server WebSocket connections share the no-context-takeover
// deflate set. Offloaded compression runs on another thread and keeps its own
// codec.
class worker_codecs final {
public:
    explicit worker_codecs(std::pmr::memory_resource* resource) noexcept;
    worker_codecs(const worker_codecs&) = delete;
    worker_codecs& operator=(const worker_codecs&) = delete;

    // `coding` is a compressing coding; identity has no encoder and throws
    // std::logic_error.
    [[nodiscard]] http_content_encode_context& encoder(http_content_coding coding);

    [[nodiscard]] websocket_deflate_codecs& websocket_deflate() noexcept {
        return websocket_deflate_;
    }

private:
    http_content_encode_context gzip_;
    http_content_encode_context deflate_;
    http_content_encode_context brotli_;
    http_content_encode_context zstd_;
    websocket_deflate_codecs websocket_deflate_;
};

}  // namespace ruvia::detail
