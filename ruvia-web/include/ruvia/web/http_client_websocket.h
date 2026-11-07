#pragma once

#include <concepts>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/web/http_client.h"
#include "ruvia/web/websocket.h"

namespace ruvia {
namespace detail {
class http_client_websocket_state;
}

// Owns one WebSocket over the registered origin's HTTP/1 Upgrade or HTTP/2/3
// Extended CONNECT. Moving preserves cold and running operations. Destruction
// aborts the tunnel; all use and destruction remain on the origin's worker.
// Origin TLS, cookies, protocol choice, transport limits and operation options
// are inherited. One receive may overlap one send; close requires both lanes,
// and failure aborts the tunnel.
// HTTP request deadlines stop at the accepted handshake. An established
// WebSocket has no total lifetime cap; explicit operation_options bound each
// operation, alongside active-read/write and close/heartbeat inactivity limits.
class http_client_websocket final {
public:
    http_client_websocket(const http_client_websocket&) = delete;
    http_client_websocket& operator=(const http_client_websocket&) = delete;
    http_client_websocket(http_client_websocket&&) noexcept;
    http_client_websocket& operator=(http_client_websocket&&) noexcept;
    ~http_client_websocket();

    // Inputs are copied before returning a cold operation; string sends text,
    // byte spans send binary, and messages retain their opcode. A per-operation
    // timeout may only tighten inherited bounds; a supplied cancellation token
    // replaces inherited user cancellation, while an empty token inherits it.
    [[nodiscard]] task<void> send(std::string_view payload, websocket_send_options options = {}, operation_options operation = {}) &;
    task<void> send(std::string_view, websocket_send_options = {}, operation_options = {}) && = delete;
    [[nodiscard]] task<void> send(std::span<const std::byte> payload, websocket_send_options options = {}, operation_options operation = {}) &;
    task<void> send(std::span<const std::byte>, websocket_send_options = {}, operation_options = {}) && = delete;
    [[nodiscard]] task<void> send(websocket_message&& message, websocket_send_options options = {}, operation_options operation = {}) &;
    task<void> send(websocket_message&&, websocket_send_options = {}, operation_options = {}) && = delete;
    template <std::same_as<std::pmr::string> string_type>
    [[nodiscard]] task<void> send(string_type&& payload, websocket_send_options options = {}, operation_options operation = {}) & {
        return write_owned(websocket_opcode::text, std::move(payload), options, std::move(operation));
    }
    template <std::same_as<std::pmr::string> string_type>
    task<void> send(string_type&&, websocket_send_options = {}, operation_options = {}) && = delete;
    // Only nullopt is EOF. The message view lasts until the next receive or close.
    [[nodiscard]] task<std::optional<websocket_message>> receive(operation_options operation = {}) &;
    task<std::optional<websocket_message>> receive(operation_options = {}) && = delete;
    [[nodiscard]] task<void> ping(std::string_view payload = {}, operation_options operation = {}) &;
    task<void> ping(std::string_view = {}, operation_options = {}) && = delete;
    [[nodiscard]] task<void> pong(std::string_view payload = {}, operation_options operation = {}) &;
    task<void> pong(std::string_view = {}, operation_options = {}) && = delete;
    template <std::same_as<std::pmr::string> string_type>
    [[nodiscard]] task<void> ping(string_type&& payload, operation_options operation = {}) & {
        return write_owned(websocket_opcode::ping, std::move(payload), {}, std::move(operation));
    }
    template <std::same_as<std::pmr::string> string_type>
    task<void> ping(string_type&&, operation_options = {}) && = delete;
    template <std::same_as<std::pmr::string> string_type>
    [[nodiscard]] task<void> pong(string_type&& payload, operation_options operation = {}) & {
        return write_owned(websocket_opcode::pong, std::move(payload), {}, std::move(operation));
    }
    template <std::same_as<std::pmr::string> string_type>
    task<void> pong(string_type&&, operation_options = {}) && = delete;
    // Close and automatic heartbeat use the builder's lifecycle policy, further
    // tightened by inherited and per-operation deadlines.
    [[nodiscard]] task<void> close(websocket_close_options options = {}, operation_options operation = {}) &;
    task<void> close(websocket_close_options = {}, operation_options = {}) && = delete;
    void abort() & noexcept;
    void abort() && = delete;
    [[nodiscard]] std::string_view subprotocol() const& RUVIA_LIFETIMEBOUND;
    std::string_view subprotocol() const&& = delete;
    [[nodiscard]] http_protocol_version protocol_version() const;
    [[nodiscard]] bool connected() const;
    [[nodiscard]] std::optional<std::uint16_t> peer_close_code() const;
    [[nodiscard]] std::string_view peer_close_reason() const& RUVIA_LIFETIMEBOUND;
    std::string_view peer_close_reason() const&& = delete;
    [[nodiscard]] const worker_handle& worker() const& RUVIA_LIFETIMEBOUND;
    const worker_handle& worker() const&& = delete;

private:
    friend class http_client_websocket_request;
    explicit http_client_websocket(std::shared_ptr<detail::http_client_websocket_state> state) noexcept;
    [[nodiscard]] task<void> write(websocket_opcode opcode, std::string_view payload,
        websocket_send_options options, operation_options operation);
    [[nodiscard]] task<void> write_owned(websocket_opcode opcode, std::pmr::string&& payload,
        websocket_send_options options, operation_options operation);
    std::shared_ptr<detail::http_client_websocket_state> state_;
};

// Every input is owned before connect() returns its cold operation. The builder
// is consumed once; headers remain application fields, never handshake fields.
class http_client_websocket_request final {
public:
    http_client_websocket_request(const http_client_websocket_request&) = delete;
    http_client_websocket_request& operator=(const http_client_websocket_request&) = delete;
    http_client_websocket_request(http_client_websocket_request&&) = default;
    http_client_websocket_request& operator=(http_client_websocket_request&&) = delete;
    http_client_websocket_request& headers(std::span<const http_header_view> fields);
    http_client_websocket_request& header(std::string_view name, std::string_view value);
    http_client_websocket_request& subprotocol(std::string_view value);
    http_client_websocket_request& deflate(websocket_client_deflate_offer offer);
    // Pong expiry runs only while receive can observe it; blocked writes retain
    // their independent liveness bound. An absent ping interval disables probing.
    http_client_websocket_request& lifecycle(websocket_lifecycle_options options);
    // Applies to incoming messages and submitted application frames; zero is invalid.
    http_client_websocket_request& max_message_bytes(std::size_t limit);
    // Connect and request deadlines bound opening only, not the session lifetime.
    [[nodiscard]] task<http_client_websocket> connect();

private:
    friend class http_client_handle;
    http_client_websocket_request(const http_client_handle& client, std::string_view path);
    [[nodiscard]] static task<http_client_websocket> connect_owned(http_client_websocket_request request);
    void require_open() const;
    http_client_handle client_;
    std::pmr::string path_;
    std::pmr::vector<http_header> headers_;
    std::pmr::vector<std::pmr::string> subprotocols_;
    websocket_client_deflate_offer deflate_{};
    websocket_lifecycle_options lifecycle_{};
    std::size_t max_message_bytes_{default_max_websocket_message_bytes};
    bool consumed_{false};
};

}  // namespace ruvia
