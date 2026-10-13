#pragma once

#include <array>
#include <cstddef>
#include <memory_resource>
#include <utility>

#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_content_encoder.h"
#include "ruvia/http/websocket_deflate_codecs.h"

namespace ruvia::detail {

class worker_codecs;

// An incremental encoder on loan from worker_codecs for one response stream.
// Destroying the lease hands the encoder back; the worker resets it and keeps
// it for the next stream, so streamed and SSE responses reuse codec state the
// way buffered responses reuse the worker's whole-buffer contexts.
class stream_encoder_lease final {
public:
    stream_encoder_lease(const stream_encoder_lease&) = delete;
    stream_encoder_lease& operator=(const stream_encoder_lease&) = delete;
    stream_encoder_lease(stream_encoder_lease&& other) noexcept
        : codecs_(std::exchange(other.codecs_, nullptr)),
          encoder_(std::exchange(other.encoder_, nullptr)) {}
    ~stream_encoder_lease() {
        release();
    }

    [[nodiscard]] http_content_encoder& encoder() noexcept {
        return *encoder_;
    }

private:
    friend class worker_codecs;

    stream_encoder_lease(worker_codecs& codecs, http_content_encoder& encoder) noexcept
        : codecs_(&codecs),
          encoder_(&encoder) {}

    void release() noexcept;

    worker_codecs* codecs_{nullptr};
    http_content_encoder* encoder_{nullptr};
};

// The web worker's reusable codec state. Everything is created on first use
// from the worker resource and lives until the worker is destroyed, so codec
// memory is sized per worker rather than per request or connection. Only
// synchronous work on the owning worker thread may borrow it: inline response
// compression takes one encoder per content coding (each coding has one fixed
// level), response streams borrow a pooled incremental encoder for their
// lifetime, and server WebSocket connections share the no-context-takeover
// deflate set. Offloaded compression runs on another thread and keeps its own
// codec.
class worker_codecs final {
public:
    explicit worker_codecs(std::pmr::memory_resource* resource) noexcept;
    ~worker_codecs();
    worker_codecs(const worker_codecs&) = delete;
    worker_codecs& operator=(const worker_codecs&) = delete;

    // `coding` is a compressing coding; identity has no encoder and throws
    // std::logic_error.
    [[nodiscard]] http_content_encode_context& encoder(http_content_coding coding);

    // A fresh incremental encoder for `coding` (a compressing coding; identity
    // throws std::logic_error): a pooled one when the worker holds any, else a
    // new one from the worker resource. Each coding keeps a bounded free list;
    // a returned encoder past that bound, or one whose reset fails, is freed.
    [[nodiscard]] stream_encoder_lease acquire_stream_encoder(http_content_coding coding);

    [[nodiscard]] websocket_deflate_codecs& websocket_deflate() noexcept {
        return websocket_deflate_;
    }

private:
    friend class stream_encoder_lease;

    // Idle stream encoders retained per coding. Deflate state alone is a few
    // hundred KB, so the bound keeps a burst of streams from pinning memory
    // for the worker's whole life.
    static constexpr std::size_t max_pooled_stream_encoders = 8;

    struct stream_encoder_pool_type final {
        std::array<http_content_encoder*, max_pooled_stream_encoders> free_{};
        std::size_t count_{0};
    };

    [[nodiscard]] stream_encoder_pool_type& stream_encoders(http_content_coding coding);
    void release_stream_encoder(http_content_encoder& encoder) noexcept;

    std::pmr::memory_resource* resource_;
    http_content_encode_context gzip_;
    http_content_encode_context deflate_;
    http_content_encode_context brotli_;
    http_content_encode_context zstd_;
    stream_encoder_pool_type stream_gzip_;
    stream_encoder_pool_type stream_deflate_;
    stream_encoder_pool_type stream_brotli_;
    stream_encoder_pool_type stream_zstd_;
    websocket_deflate_codecs websocket_deflate_;
};

inline void stream_encoder_lease::release() noexcept {
    if (encoder_ != nullptr) {
        codecs_->release_stream_encoder(*std::exchange(encoder_, nullptr));
        codecs_ = nullptr;
    }
}

}  // namespace ruvia::detail
