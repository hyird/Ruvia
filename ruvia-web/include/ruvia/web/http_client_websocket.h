#pragma once

#include <concepts>
#include <cstdint>
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
class http_client_websocket_message_writer;

// Owns one WebSocket over the registered origin's HTTP/1 Upgrade or HTTP/2/3
// Extended CONNECT. Moving preserves cold and running operations. Destruction
// aborts the tunnel; all use and destruction remain on the origin's worker.
// Origin TLS, cookies, protocol choice, transport limits and operation options
// are inherited. Operations mirror the server websocket: one read may overlap
// one output operation, and close() claims only the write lane.
// HTTP request deadlines stop at the accepted handshake. An established
// WebSocket has no total lifetime cap; explicit operation_options bound each
// operation, alongside active-read/write and close/heartbeat inactivity limits.
// A per-operation timeout may only tighten inherited bounds; a supplied
// cancellation token replaces inherited user cancellation, while an empty
// token inherits it.
class http_client_websocket final {
public:
    http_client_websocket(const http_client_websocket&) = delete;
    http_client_websocket& operator=(const http_client_websocket&) = delete;
    http_client_websocket(http_client_websocket&&) noexcept;
    http_client_websocket& operator=(http_client_websocket&&) noexcept;
    ~http_client_websocket();

    // Only nullopt is EOF: a close handshake completed by either side, or any
    // end once this side's close() has sent its Close. Other failures abort
    // the session and throw http_client_error. The message view lasts until
    // the next read() or close(). A stop request or timeout in `operation`
    // ends only this read with http_client_error (cancelled or timeout);
    // buffered input, the read lane and the session stay usable. Once the
    // peer Close has arrived the read finishes the handshake first.
    [[nodiscard]] task<std::optional<websocket_message>> read(operation_options operation = {}) &;
    task<std::optional<websocket_message>> read(operation_options = {}) && = delete;

    // Inputs are copied before returning a cold operation; messages retain
    // their opcode. Output operations share one write lane. A stop request,
    // timeout or failure aborts the session (a partly written frame cannot be
    // withdrawn); an already-stopped token fails before any output.
    [[nodiscard]] task<void> text(std::string_view payload, websocket_send_options options = {}, operation_options operation = {}) &;
    task<void> text(std::string_view, websocket_send_options = {}, operation_options = {}) && = delete;
    template <std::same_as<std::pmr::string> string_type>
    [[nodiscard]] task<void> text(string_type&& payload, websocket_send_options options = {}, operation_options operation = {}) & {
        return write_owned(websocket_opcode::text, std::move(payload), options, std::move(operation));
    }
    template <std::same_as<std::pmr::string> string_type>
    task<void> text(string_type&&, websocket_send_options = {}, operation_options = {}) && = delete;
    [[nodiscard]] task<void> binary(std::string_view payload, websocket_send_options options = {}, operation_options operation = {}) &;
    task<void> binary(std::string_view, websocket_send_options = {}, operation_options = {}) && = delete;
    [[nodiscard]] task<void> binary(std::span<const std::byte> payload, websocket_send_options options = {}, operation_options operation = {}) &;
    task<void> binary(std::span<const std::byte>, websocket_send_options = {}, operation_options = {}) && = delete;
    template <std::same_as<std::pmr::string> string_type>
    [[nodiscard]] task<void> binary(string_type&& payload, websocket_send_options options = {}, operation_options operation = {}) & {
        return write_owned(websocket_opcode::binary, std::move(payload), options, std::move(operation));
    }
    template <std::same_as<std::pmr::string> string_type>
    task<void> binary(string_type&&, websocket_send_options = {}, operation_options = {}) && = delete;
    [[nodiscard]] task<void> pong(std::string_view payload = {}, operation_options operation = {}) &;
    task<void> pong(std::string_view = {}, operation_options = {}) && = delete;
    template <std::same_as<std::pmr::string> string_type>
    [[nodiscard]] task<void> pong(string_type&& payload, operation_options operation = {}) & {
        return write_owned(websocket_opcode::pong, std::move(payload), {}, std::move(operation));
    }
    template <std::same_as<std::pmr::string> string_type>
    task<void> pong(string_type&&, operation_options = {}) && = delete;
    [[nodiscard]] task<void> ping(std::string_view payload = {}, operation_options operation = {}) &;
    task<void> ping(std::string_view = {}, operation_options = {}) && = delete;
    template <std::same_as<std::pmr::string> string_type>
    [[nodiscard]] task<void> ping(string_type&& payload, operation_options operation = {}) & {
        return write_owned(websocket_opcode::ping, std::move(payload), {}, std::move(operation));
    }
    template <std::same_as<std::pmr::string> string_type>
    task<void> ping(string_type&&, operation_options = {}) && = delete;
    [[nodiscard]] task<void> send(websocket_message&& message, websocket_send_options options = {}, operation_options operation = {}) &;
    task<void> send(websocket_message&&, websocket_send_options = {}, operation_options = {}) && = delete;
    // Claims the write lane only, so it may start while a read is pending:
    // that read returns nullopt once the handshake ends. Without a pending
    // read, close() itself reads until the peer Close and a read started
    // meanwhile throws std::logic_error. The builder's lifecycle policy bounds
    // the handshake (its timeout throws http_client_error timeout), further
    // tightened by inherited and per-operation deadlines.
    [[nodiscard]] task<void> close(websocket_close_options options = {}, operation_options operation = {}) &;
    task<void> close(websocket_close_options = {}, operation_options = {}) && = delete;
    void abort() & noexcept;
    void abort() && = delete;
    // A data message sent as several frames, with the contract of
    // websocket::begin_text() / begin_binary().
    [[nodiscard]] http_client_websocket_message_writer begin_text(websocket_send_options options = {}) &;
    http_client_websocket_message_writer begin_text(websocket_send_options = {}) && = delete;
    [[nodiscard]] http_client_websocket_message_writer begin_binary(websocket_send_options options = {}) &;
    http_client_websocket_message_writer begin_binary(websocket_send_options = {}) && = delete;

    // Session state, read like the server websocket's accessors: use on the
    // origin's worker only (other workers terminate); a moved-from owner
    // reports an empty subprotocol, no peer Close, none, zero and false.
    [[nodiscard]] std::string_view subprotocol() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view subprotocol() const&& = delete;
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] std::optional<std::uint16_t> peer_close_code() const noexcept;
    [[nodiscard]] std::string_view peer_close_reason() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view peer_close_reason() const&& = delete;
    [[nodiscard]] websocket_termination termination() const noexcept;
    [[nodiscard]] std::uint64_t bytes_received() const noexcept;
    [[nodiscard]] std::uint64_t bytes_sent() const noexcept;

    // Transport metadata of a live owner; a moved-from owner or another
    // worker throws std::logic_error.
    [[nodiscard]] http_protocol_version protocol_version() const;
    [[nodiscard]] const http_client_connection_info& connection_info() const& RUVIA_LIFETIMEBOUND;
    const http_client_connection_info& connection_info() const&& = delete;
    [[nodiscard]] const worker_handle& worker() const& RUVIA_LIFETIMEBOUND;
    const worker_handle& worker() const&& = delete;

private:
    friend class http_client_websocket_request;
    explicit http_client_websocket(std::shared_ptr<detail::http_client_websocket_state> state) noexcept;
    [[nodiscard]] task<void> write(websocket_opcode opcode, std::string_view payload,
        websocket_send_options options, operation_options operation,
        detail::websocket_write_extent extent = detail::websocket_write_extent::message);
    [[nodiscard]] http_client_websocket_message_writer begin_message(websocket_opcode opcode, websocket_send_options options);
    [[nodiscard]] task<void> write_owned(websocket_opcode opcode, std::pmr::string&& payload,
        websocket_send_options options, operation_options operation);
    std::shared_ptr<detail::http_client_websocket_state> state_;
};

// The client counterpart of websocket_message_writer, with the same contract.
// It keeps the session alive; use and destroy it on the origin's worker.
class http_client_websocket_message_writer final {
public:
    http_client_websocket_message_writer(http_client_websocket_message_writer&& other) noexcept;
    http_client_websocket_message_writer& operator=(http_client_websocket_message_writer&&) = delete;
    http_client_websocket_message_writer(const http_client_websocket_message_writer&) = delete;
    http_client_websocket_message_writer& operator=(const http_client_websocket_message_writer&) = delete;
    ~http_client_websocket_message_writer();

    [[nodiscard]] task<void> write(std::string_view chunk, operation_options operation = {}) &;
    task<void> write(std::string_view, operation_options = {}) && = delete;
    [[nodiscard]] task<void> write(std::span<const std::byte> chunk, operation_options operation = {}) &;
    task<void> write(std::span<const std::byte>, operation_options = {}) && = delete;
    [[nodiscard]] task<void> finish(std::string_view last_chunk = {}, operation_options operation = {}) &;
    task<void> finish(std::string_view = {}, operation_options = {}) && = delete;
    [[nodiscard]] task<void> finish(std::span<const std::byte> last_chunk, operation_options operation = {}) &;
    task<void> finish(std::span<const std::byte>, operation_options = {}) && = delete;
    void abort() & noexcept;
    void abort() && = delete;

private:
    friend class http_client_websocket;
    http_client_websocket_message_writer(std::shared_ptr<detail::http_client_websocket_state> state,
        std::uint64_t message_id, websocket_opcode opcode, websocket_send_options options) noexcept;
    [[nodiscard]] task<void> submit(std::string_view chunk, bool final, operation_options operation);
    [[nodiscard]] bool message_open() const noexcept;

    std::shared_ptr<detail::http_client_websocket_state> state_;
    std::uint64_t message_id_;
    websocket_opcode opcode_;
    websocket_send_options options_;
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
    http_client_websocket_request& configure(http_client_request_options options);
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
    http_client_request_options request_options_;
    bool consumed_{false};
};

}  // namespace ruvia
