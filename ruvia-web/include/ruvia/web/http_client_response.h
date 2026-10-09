#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

#include "ruvia/core/scoped_operation.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_priority.h"
#include "ruvia/web/http_client_informational_response.h"
#include "ruvia/web/http_client_response_bytes.h"

namespace ruvia {

namespace detail {
class http_client_pool;
class http_client_response_state;
class http_capsule_stream_state;
}  // namespace detail

class response_stream_writer;
class worker_handle;

// A borrow-only facade embedded in http_client_response. It cannot be detached,
// independently destroyed, or moved away from the response. Body operations
// bind directly to the response's address-stable transport state, so moving
// their public response owner does not invalidate a cold or running operation.
class http_client_response_body final {
public:
    http_client_response_body(const http_client_response_body&) = delete;
    http_client_response_body& operator=(const http_client_response_body&) = delete;
    http_client_response_body(http_client_response_body&&) = delete;
    http_client_response_body& operator=(http_client_response_body&&) = delete;

    // The returned view remains valid until the next body operation. A null
    // optional is the only end-of-body signal; an empty data chunk is never
    // returned. Reads are linear and concurrent operations are rejected.
    [[nodiscard]] scoped_operation<std::optional<std::span<const std::byte>>> read() &;
    scoped_operation<std::optional<std::span<const std::byte>>> read() && = delete;

    // Reads one chunk as characters, without decoding or charset validation.
    // Encoded characters may straddle chunks. Uses the same linear read lane.
    [[nodiscard]] scoped_operation<std::optional<std::string_view>> text() &;
    scoped_operation<std::optional<std::string_view>> text() && = delete;

    // Collects the unread remainder of this same stream. max_bytes is a caller
    // bound in addition to the origin's transport bound.
    [[nodiscard]] scoped_operation<http_client_response_bytes> read_all(
        std::size_t max_bytes = default_max_buffered_body_bytes) &;
    scoped_operation<http_client_response_bytes> read_all(
        std::size_t = default_max_buffered_body_bytes) && = delete;

    // Copies this same stream into a controller response stream with natural
    // backpressure. This is the common forwarding path for both small and
    // long-lived upstream responses.
    [[nodiscard]] scoped_operation<void> pipe_to(response_stream_writer& output) &;
    scoped_operation<void> pipe_to(response_stream_writer&) && = delete;

    [[nodiscard]] bool complete() const noexcept;

private:
    friend class http_client_response;
    friend class detail::http_client_pool;

    ~http_client_response_body() = default;

    explicit http_client_response_body(detail::http_client_response_state* state_value) noexcept
        : state_(state_value) {}

    detail::http_client_response_state* state_{nullptr};
};

// Owns worker-affine response storage independently of its client transport.
// May survive client shutdown/destruction on that worker, but must be destroyed
// before the bound event_loop retires. Metadata views borrow this response;
// body chunks remain valid until the next body operation.
class http_client_response final {
public:
    http_client_response(const http_client_response&) = delete;
    http_client_response& operator=(const http_client_response&) = delete;
    // Body operations bind to the address-stable response state, so the public
    // owner remains movable while an operation is cold or running.
    http_client_response(http_client_response&& other) noexcept;
    http_client_response& operator=(http_client_response&& other) noexcept;
    ~http_client_response();

    [[nodiscard]] std::span<const http_client_informational_response> informational_responses() const& noexcept;
    std::span<const http_client_informational_response> informational_responses() const&& = delete;
    [[nodiscard]] http_status_code status() const noexcept;
    [[nodiscard]] http_protocol_version protocol_version() const noexcept;
    // Queues RFC 9218 PRIORITY_UPDATE on a live HTTP/2 or HTTP/3 response.
    // Worker-affine; the connection driver owns writing the queued frame.
    void reprioritize(http_priority priority) &;
    void reprioritize(http_priority) && = delete;
    [[nodiscard]] std::span<const http_header> headers() const& noexcept;
    [[nodiscard]] std::span<const http_header> headers() const&& = delete;
    [[nodiscard]] std::span<const http_header> trailers() const& noexcept;
    [[nodiscard]] std::span<const http_header> trailers() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& noexcept;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view) const&& = delete;
    [[nodiscard]] std::optional<std::string_view> trailer(std::string_view name) const& noexcept;
    [[nodiscard]] std::optional<std::string_view> trailer(std::string_view) const&& = delete;
    [[nodiscard]] http_client_response_body& body() & noexcept {
        return body_;
    }
    [[nodiscard]] const http_client_response_body& body() const& = delete;
    [[nodiscard]] http_client_response_body& body() && = delete;

private:
    friend class detail::http_client_pool;
    friend class http_client_exchange;
    friend class http_client_tunnel;
    friend class http_capsule_stream;
    friend class http_datagram_stream;
    friend class detail::http_capsule_stream_state;
    friend class http_client_push;
    friend class http_client_request_body_writer;

    explicit http_client_response(detail::http_client_pool& pool);
    http_client_response(detail::http_client_response_state* state_value, bool retain) noexcept;
    void release() noexcept;

    detail::http_client_response_state* state_{nullptr};
    http_client_response_body body_{state_};
    bool consumer_{true};
};

}  // namespace ruvia
