#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/core/scoped_operation.h"
#include "ruvia/web/http_capsule_stream.h"
#include "ruvia/web/http_client_response.h"
#include "ruvia/web/http_datagram_stream.h"

namespace ruvia {
class http_udp_tunnel;
namespace detail {
class http_client_pool;
class http_capsule_stream_state;
}  // namespace detail

// Owns an established CONNECT byte stream on its client worker. Metadata borrows
// this owner; a read view lasts until the next read. Inputs are copied before a
// cold write is returned. One read and one write/finish may coexist. Moving the
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
    // Consumes an accepted CONNECT-UDP tunnel after validated negotiation.
    [[nodiscard]] http_datagram_stream datagrams(http_datagram_config config = {}) &&;
    http_datagram_stream datagrams(http_datagram_config = {}) & = delete;
    [[nodiscard]] http_udp_tunnel udp(http_datagram_config config = {}) &&;
    http_udp_tunnel udp(http_datagram_config = {}) & = delete;
    void reprioritize(http_priority priority) & {
        response_.reprioritize(priority);
    }
    void reprioritize(http_priority) && = delete;

private:
    friend class detail::http_client_pool;
    friend class detail::http_capsule_stream_state;
    friend class http_capsule_stream;
    friend class http_datagram_stream;
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
}  // namespace ruvia
