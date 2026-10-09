#pragma once

#include <span>
#include <string_view>

#include "ruvia/core/scoped_operation.h"
#include "ruvia/web/http_client_response.h"
#include "ruvia/web/http_client_upload_config.h"

namespace ruvia {
namespace detail {
class http_client_pool;
class http_client_response_state;
}  // namespace detail

// One upload lane embedded in its exchange. Each chunk/trailer input is owned
// before return; completing write() means transport accepted the entire chunk.
// Only one write/end operation may exist at a time. Chunk size is bounded by
// max_chunk_bytes_; split larger sources into sequential writes.
class http_client_request_body_writer final {
public:
    http_client_request_body_writer(const http_client_request_body_writer&) = delete;
    http_client_request_body_writer& operator=(const http_client_request_body_writer&) = delete;
    [[nodiscard]] scoped_operation<void> write(std::span<const std::byte> bytes) &;
    scoped_operation<void> write(std::span<const std::byte>) && = delete;
    [[nodiscard]] scoped_operation<void> write(std::string_view bytes) &;
    scoped_operation<void> write(std::string_view) && = delete;
    [[nodiscard]] scoped_operation<void> end(std::span<const http_header_view> trailers = {}) &;
    scoped_operation<void> end(std::span<const http_header_view> = {}) && = delete;
    [[nodiscard]] bool complete() const noexcept;

private:
    friend class http_client_exchange;
    explicit http_client_request_body_writer(detail::http_client_response_state* state_value) noexcept
        : state_(state_value) {}
    ~http_client_request_body_writer() = default;
    detail::http_client_response_state* state_;
};

// Owns a streaming request and its eventual response on one worker. Moving
// preserves operations because their state is address stable. Destruction
// abandons unfinished upload; client shutdown wakes and joins its producer.
class http_client_exchange final {
public:
    http_client_exchange(const http_client_exchange&) = delete;
    http_client_exchange& operator=(const http_client_exchange&) = delete;
    http_client_exchange(http_client_exchange&& other) noexcept;
    http_client_exchange& operator=(http_client_exchange&& other) noexcept;
    ~http_client_exchange();
    [[nodiscard]] http_client_request_body_writer& body() & noexcept {
        return body_;
    }
    http_client_request_body_writer& body() && = delete;
    // Can run concurrently with the upload lane to observe an early final
    // response. Transfers response ownership once, when actually started.
    [[nodiscard]] scoped_operation<http_client_response> response() &;
    scoped_operation<http_client_response> response() && = delete;

private:
    friend class detail::http_client_pool;
    explicit http_client_exchange(http_client_response response) noexcept;
    void release() noexcept;
    static task<http_client_response> receive_owned(http_client_response pin);
    http_client_response response_;
    http_client_request_body_writer body_;
};
}  // namespace ruvia
