#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/core/operation_options.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_connect_udp.h"
#include "ruvia/http/http_connection_advertisement.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_push.h"
#include "ruvia/web/http_capsule_stream.h"
#include "ruvia/web/http_client_response.h"
#include "ruvia/web/http_client_types.h"
#include "ruvia/web/http_datagram_stream.h"

namespace ruvia {

class http_udp_tunnel;

namespace detail {
class http_client_pool;
class http_client_registry;
class http_client_response_state;
class http_client_advertisement_queue;
struct http_client_advertisement_state;
class http_capsule_stream_state;
}  // namespace detail

// Owns an established CONNECT byte stream on its client worker. Metadata borrows
// this owner; a read view lasts until the next read starts. Inputs are copied before a
// cold write is returned. One running read and one running write/finish may coexist; cold operations do not reserve either lane. Moving the
// owner preserves operations. Destruction aborts both directions; shutdown joins
// transport drivers. finish() closes only the local sending direction and is
// idempotent after successful completion, including normal transport retirement.
class http_client_tunnel final {
public:
    http_client_tunnel(const http_client_tunnel&) = delete;
    http_client_tunnel& operator=(const http_client_tunnel&) = delete;
    http_client_tunnel(http_client_tunnel&& other) noexcept;
    http_client_tunnel& operator=(http_client_tunnel&& other) noexcept;
    ~http_client_tunnel();
    [[nodiscard]] http_status_code status() const noexcept {
        return response_.status();
    }
    [[nodiscard]] http_protocol_version protocol_version() const noexcept {
        return response_.protocol_version();
    }
    [[nodiscard]] std::span<const http_header> headers() const& noexcept {
        return response_.headers();
    }
    std::span<const http_header> headers() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& noexcept {
        return response_.header(name);
    }
    std::optional<std::string_view> header(std::string_view) const&& = delete;
    [[nodiscard]] std::span<const http_client_informational_response> informational_responses() const& noexcept {
        return response_.informational_responses();
    }
    std::span<const http_client_informational_response> informational_responses() const&& = delete;
    [[nodiscard]] scoped_operation<std::optional<std::span<const std::byte>>> read() &;
    scoped_operation<std::optional<std::span<const std::byte>>> read() && = delete;
    [[nodiscard]] scoped_operation<void> write(std::string_view bytes) &;
    scoped_operation<void> write(std::string_view) && = delete;
    [[nodiscard]] scoped_operation<void> write(std::span<const std::byte> bytes) &;
    scoped_operation<void> write(std::span<const std::byte>) && = delete;
    // A successful finish stays idempotent after transport retirement.
    [[nodiscard]] scoped_operation<void> finish() &;
    scoped_operation<void> finish() && = delete;
    void abort() & noexcept;
    void abort() && = delete;
    // Transfers tunnel ownership to the Capsule Protocol adapter.
    [[nodiscard]] http_capsule_stream capsules(http_capsule_config config = {}) &&;
    http_capsule_stream capsules(http_capsule_config = {}) & = delete;
    // Checked ownership transfers for accepted, negotiated datagram/CONNECT-UDP
    // tunnels. Ordinary CONNECT throws std::logic_error without consuming this
    // owner. The Capsule Protocol limits apply directly to encoded datagrams.
    [[nodiscard]] http_datagram_stream datagrams(http_capsule_config config = {}) &&;
    http_datagram_stream datagrams(http_capsule_config = {}) & = delete;
    [[nodiscard]] http_udp_tunnel udp(http_capsule_config config = {}) &&;
    http_udp_tunnel udp(http_capsule_config = {}) & = delete;

private:
    friend class detail::http_client_pool;
    friend class detail::http_capsule_stream_state;
    friend class http_capsule_stream;
    explicit http_client_tunnel(http_client_response response) noexcept;
    void release() noexcept;
    http_client_response response_;
};

// A rejected CONNECT keeps its ordinary HTTP status, headers and response body.
// Only a successful handshake exposes a tunnel.
class http_client_tunnel_result final {
public:
    http_client_tunnel_result(const http_client_tunnel_result&) = delete;
    http_client_tunnel_result& operator=(const http_client_tunnel_result&) = delete;
    http_client_tunnel_result(http_client_tunnel_result&&) noexcept = default;
    http_client_tunnel_result& operator=(http_client_tunnel_result&&) noexcept = default;
    [[nodiscard]] http_client_tunnel* tunnel() & noexcept {
        return std::get_if<http_client_tunnel>(&value_);
    }
    http_client_tunnel* tunnel() && = delete;
    [[nodiscard]] http_client_response* response() & noexcept {
        return std::get_if<http_client_response>(&value_);
    }
    http_client_response* response() && = delete;

private:
    friend class detail::http_client_pool;
    friend class detail::http_capsule_stream_state;
    friend class http_capsule_stream;
    friend class http_datagram_stream;
    explicit http_client_tunnel_result(http_client_tunnel tunnel) noexcept
        : value_(std::move(tunnel)) {}
    explicit http_client_tunnel_result(http_client_response response) noexcept
        : value_(std::move(response)) {}
    std::variant<http_client_tunnel, http_client_response> value_;
};

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
