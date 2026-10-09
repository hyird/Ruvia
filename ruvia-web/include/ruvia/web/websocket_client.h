#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/operation_options.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/tcp_socket_options.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/websocket_client_negotiation.h"
#include "ruvia/http/websocket_protocol.h"
#include "ruvia/web/http3_qpack_config.h"
#include "ruvia/web/tls_peer_verification.h"
#include "ruvia/web/websocket.h"

namespace ruvia {

enum class websocket_scheme : std::uint8_t {
    ws,
    wss,
};

enum class websocket_client_protocol : std::uint8_t { http1,
    http2,
    http3 };

struct websocket_client_config final {
    websocket_scheme scheme_{websocket_scheme::wss};
    websocket_client_protocol protocol_{websocket_client_protocol::http1};
    // Validated unbracketed transport host; DNS names may retain one trailing dot.
    std::string host_{};
    std::optional<std::uint16_t> port_{};
    std::string target_{"/"};
    std::vector<std::pair<std::string, std::string>> headers_{};
    std::vector<std::string> subprotocols_{};
    websocket_client_deflate_offer deflate_{};
    int compression_level_{6};
    http3_qpack_config qpack_{};
    std::size_t max_message_bytes_{default_max_websocket_message_bytes};
    std::chrono::milliseconds connect_timeout_{5000};
    std::optional<std::chrono::milliseconds> read_timeout_{};
    std::optional<std::chrono::milliseconds> write_timeout_{30000};
    // After a local Close is sent, the maximum time to wait for the peer Close.
    // nullopt disables this guard.
    std::optional<std::chrono::milliseconds> close_handshake_timeout_{5000};
    websocket_heartbeat_config heartbeat_{};
    tls_peer_verification_policy tls_peer_verification_{tls_peer_verification_policy::verify};
    tcp_no_delay_policy tcp_no_delay_{tcp_no_delay_policy::enable};
    tcp_keep_alive_policy tcp_keep_alive_{tcp_keep_alive_policy::enable};
    std::string ca_file_{};
    std::string certificate_chain_file_{};
    std::string private_key_file_{};
    std::string private_key_password_{};
    std::string user_agent_{"Ruvia"};
};

class websocket_client_error final : public std::runtime_error {
public:
    enum class code_type : std::uint8_t {
        invalid_config,
        invalid_state,
        timeout,
        cancelled,
        resolve_failed,
        connect_failed,
        tls_failed,
        handshake_rejected,
        io_error,
        protocol_error,
        message_too_large,
        closing,
    };

    websocket_client_error(code_type code, std::string_view message)
        : std::runtime_error(std::string(message)),
          code_(code) {}

    [[nodiscard]] code_type code() const noexcept {
        return code_;
    }

private:
    code_type code_;
};

namespace detail {
class websocket_client_state;
}

class websocket_client_handle final {
public:
    websocket_client_handle(const websocket_client_handle& other) noexcept;
    websocket_client_handle& operator=(const websocket_client_handle&) = delete;

    [[nodiscard]] websocket_client_handle with_options(operation_options options) const;
    [[nodiscard]] scoped_operation<std::optional<websocket_message>> read() const;
    [[nodiscard]] scoped_operation<void> text(std::string_view payload, websocket_send_options options = {}) const;
    [[nodiscard]] scoped_operation<void> binary(std::string_view payload, websocket_send_options options = {}) const;
    [[nodiscard]] scoped_operation<void> ping(std::string_view payload = {}) const;
    [[nodiscard]] scoped_operation<void> pong(std::string_view payload) const;
    [[nodiscard]] scoped_operation<void> close(websocket_close_options options) const;
    void abort() noexcept;

private:
    friend class detail::websocket_client_state;
    websocket_client_handle(std::shared_ptr<detail::websocket_client_state> state_value,
        ::ruvia::operation_scope& scope, operation_options options) noexcept;
    static void expire_capability(void* target) noexcept;

    std::shared_ptr<detail::websocket_client_state> state_;
    operation_options options_;
    scoped_capability_registration registration_;
};

// One websocket connection bound to one Ruvia event loop. Construction performs
// no I/O; connect() is lazy and must be started on the bound loop. The connection
// is worker-affine and never migrates between event loops.
class websocket_client final {
public:
    websocket_client(event_loop loop, const websocket_client_config& config);
    ~websocket_client();

    websocket_client(const websocket_client&) = delete;
    websocket_client& operator=(const websocket_client&) = delete;
    websocket_client(websocket_client&&) = delete;
    websocket_client& operator=(websocket_client&&) = delete;

    // Connect tasks, handles, and operations all depend on this client's open
    // lifecycle. A temporary would request shutdown immediately after creating
    // one, so operation-producing APIs are lvalue-only.
    [[nodiscard]] task<void> connect() &;
    task<void> connect() && = delete;
    [[nodiscard]] websocket_client_handle with_options(operation_options options) const&;
    websocket_client_handle with_options(operation_options) const&& = delete;
    [[nodiscard]] scoped_operation<std::optional<websocket_message>> read() const&;
    scoped_operation<std::optional<websocket_message>> read() const&& = delete;
    [[nodiscard]] scoped_operation<void> text(std::string_view payload, websocket_send_options options = {}) const&;
    scoped_operation<void> text(std::string_view, websocket_send_options = {}) const&& = delete;
    [[nodiscard]] scoped_operation<void> binary(std::string_view payload, websocket_send_options options = {}) const&;
    scoped_operation<void> binary(std::string_view, websocket_send_options = {}) const&& = delete;
    [[nodiscard]] scoped_operation<void> ping(std::string_view payload = {}) const&;
    scoped_operation<void> ping(std::string_view = {}) const&& = delete;
    [[nodiscard]] scoped_operation<void> pong(std::string_view payload) const&;
    scoped_operation<void> pong(std::string_view) const&& = delete;
    [[nodiscard]] scoped_operation<void> close(websocket_close_options options) const&;
    scoped_operation<void> close(websocket_close_options) const&& = delete;

    // Idempotent and callable from any thread. This only requests immediate
    // transport termination. Graceful RFC 6455 close uses the typed overload
    // above: co_await client.close({...}). Use shutdown() when completion must
    // be awaited.
    void abort() noexcept;
    // Requests immediate transport shutdown, joins an in-flight connect and
    // all worker-bound operations, and completes on the bound loop after
    // teardown has finished.
    [[nodiscard]] task<void> shutdown() &;
    task<void> shutdown() && = delete;

    [[nodiscard]] bool connected() const;
    [[nodiscard]] std::string_view subprotocol() const&;
    std::string_view subprotocol() const&& = delete;
    [[nodiscard]] const worker_handle& worker() const& noexcept;
    const worker_handle& worker() const&& = delete;

private:
    std::shared_ptr<detail::websocket_client_state> state_;
};

}  // namespace ruvia
