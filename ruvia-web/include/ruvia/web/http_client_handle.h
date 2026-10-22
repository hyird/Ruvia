#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>

#include "ruvia/core/operation_options.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_client_tunnel_request_view.h"
#include "ruvia/http/http_connect_udp.h"
#include "ruvia/http/http_connection_advertisement.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_push.h"
#include "ruvia/web/http_client_response.h"
#include "ruvia/web/http_client_tunnel.h"
#include "ruvia/web/http_client_types.h"

namespace ruvia {

namespace detail {
class http_client_pool;
class http_client_registry;
class http_client_response_state;
class http_client_advertisement_queue;
struct http_client_advertisement_state;
}  // namespace detail

// A worker-affine, independently owned observation. Its metadata survives
// client shutdown and later observations, and expires when this owner dies.
// Destroy before the event_loop retires. A slot identifies the fixed client
// pool position, not a persistent connection across reconnects.
class http_client_advertisement final {
public:
    http_client_advertisement(http_client_advertisement&& other) noexcept;
    http_client_advertisement& operator=(http_client_advertisement&& other) noexcept;
    http_client_advertisement(const http_client_advertisement&) = delete;
    http_client_advertisement& operator=(const http_client_advertisement&) = delete;
    ~http_client_advertisement();

    [[nodiscard]] http_protocol_version protocol_version() const noexcept;
    [[nodiscard]] std::size_t connection_slot() const noexcept;
    [[nodiscard]] const http_origin_advertisement* origins() const& noexcept;
    const http_origin_advertisement* origins() const&& = delete;
    [[nodiscard]] const http_alternative_service_advertisement* alternative_service() const& noexcept;
    const http_alternative_service_advertisement* alternative_service() const&& = delete;

private:
    friend class detail::http_client_advertisement_queue;
    explicit http_client_advertisement(detail::http_client_advertisement_state* state_value) noexcept
        : state_(state_value) {}
    detail::http_client_advertisement_state* state_{};
};

// An accepted promise owns its copied request and response storage on the
// client worker. Destruction cancels unfinished push. Like an ordinary response,
// it can survive client shutdown but must retire before the event_loop.
class http_client_push final {
public:
    http_client_push(const http_client_push&) = delete;
    http_client_push& operator=(const http_client_push&) = delete;
    http_client_push(http_client_push&&) noexcept;
    http_client_push& operator=(http_client_push&&) noexcept;
    ~http_client_push();
    [[nodiscard]] const http_push_request& request() const&;
    const http_push_request& request() const&& = delete;
    // Transfers the response once, when the operation is started. Promise
    // metadata remains available through this push until it is destroyed.
    [[nodiscard]] scoped_operation<http_client_response> response() &;
    scoped_operation<http_client_response> response() && = delete;

private:
    friend class detail::http_client_pool;
    explicit http_client_push(http_client_response response) noexcept;
    void release() noexcept;
    static task<http_client_response> receive_owned(http_client_response pin);
    http_client_response response_;
};

// One upload lane embedded in its exchange. Each chunk/trailer input is owned
// before return; completing write() means transport accepted the entire chunk.
// Only one write/end operation may run at a time. Cold operations can coexist
// and claim the output lane when awaited. Chunk size is bounded by
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

class context;

class http_client_handle final {
public:
    http_client_handle(const http_client_handle& other);
    http_client_handle& operator=(const http_client_handle&) = delete;

    [[nodiscard]] http_client_handle with_options(operation_options options) const;
    [[nodiscard]] scoped_operation<http_client_response> send(
        const http_client_request_view& request) const;
    [[nodiscard]] scoped_operation<http_client_exchange> open_request(
        const http_client_request_view& head, http_client_upload_config upload = {}) const;
    [[nodiscard]] scoped_operation<http_client_tunnel_result> open_tunnel(const http_client_tunnel_request_view& request, http_client_tunnel_config config = {}) const;
    // Negotiates RFC 9298 and returns an accepted tunnel or ordinary rejection.
    // The proxy authority comes from this registered origin. Capsule-Protocol
    // and HTTP/1 Upgrade fields are driver-owned. Use accepted.tunnel()->udp().
    [[nodiscard]] scoped_operation<http_client_tunnel_result> open_udp_tunnel(const http_client_udp_tunnel_request_view& request, http_client_tunnel_config config = {}) const;
    [[nodiscard]] http_client_stats stats() const;
    [[nodiscard]] std::optional<http_client_advertisement> next_advertisement() const;
    [[nodiscard]] std::optional<http_client_push> next_push() const;
    [[nodiscard]] std::string_view host() const&;
    [[nodiscard]] std::string_view host() const&& = delete;
    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] http_scheme scheme() const;

private:
    friend class detail::http_client_registry;
    friend class context;
    friend class web_worker_context;
    http_client_handle(detail::http_client_pool& pool, std::pmr::memory_resource* resource,
        ::ruvia::operation_scope& scope) noexcept;
    http_client_handle(detail::http_client_pool& pool, std::pmr::memory_resource* resource,
        ::ruvia::operation_scope& scope, operation_options options) noexcept;
    static void expire_capability(void* target) noexcept;

    detail::http_client_pool* pool_{nullptr};
    std::pmr::memory_resource* resource_{nullptr};
    operation_options options_;
    scoped_capability_registration registration_;
};

}  // namespace ruvia
