#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/websocket_client_negotiation.h"
#include "ruvia/http/websocket_protocol.h"
#include "ruvia/web/client_tls_config.h"
#include "ruvia/web/websocket.h"

namespace ruvia {

enum class websocket_scheme : std::uint8_t {
    ws,
    wss,
};

enum class websocket_client_protocol : std::uint8_t { http1,
    http2 };

// span 及元素中的文本均为借用；websocket_client 构造返回前复制配置。
// 独立保存 config 时，数组及文本 owner 必须存活到构造入口消费完成。
struct websocket_client_config final {
    websocket_scheme scheme_{websocket_scheme::wss};
    websocket_client_protocol protocol_{websocket_client_protocol::http1};
    // Validated unbracketed transport host; DNS names may retain one trailing dot.
    borrowed_text host_{};
    std::optional<std::uint16_t> port_{};
    borrowed_text target_{"/"};
    std::span<const http_header_view> headers_{};
    std::span<const borrowed_text> subprotocols_{};
    websocket_client_deflate_offer deflate_{};
    int compression_level_{6};
    std::size_t max_message_bytes_{default_max_websocket_message_bytes};
    std::chrono::milliseconds connect_timeout_{5000};
    std::optional<std::chrono::milliseconds> read_timeout_{};
    std::optional<std::chrono::milliseconds> write_timeout_{30000};
    // After a local Close is sent, the maximum time to wait for the peer Close.
    // After the close handshake completes, the maximum time to wait for the
    // server to close the transport first (RFC 6455 section 7.1.1); the client
    // then closes it itself without failing read() or close(). Each wait has
    // its own bound, so close() can take up to twice this value. nullopt
    // disables both guards.
    std::optional<std::chrono::milliseconds> close_handshake_timeout_{5000};
    // Pongs are matched by read(); a pong timeout retires the client only while
    // a read is active, and an expired window restarts when the next read begins.
    // A write (application message or Ping) that has not completed within
    // ping_interval + pong_timeout since the last activity retires the client
    // with `timeout`, independently of write_timeout_.
    websocket_heartbeat_config heartbeat_{};
    // Used for wss; client_tls_mode::disabled is rejected (select ws instead).
    client_tls_config tls_{};
    client_tcp_config tcp_{};
    borrowed_text user_agent_{"Ruvia"};
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

// Worker-affine capability over one connected websocket_client. Handles and
// their operations borrow the client; its lifecycle expires every handle and
// joins every operation before retirement. Write payloads (text, binary, ping,
// pong) are copied into the client PMR before returning. Cold operations may
// coexist; each read/write/close claims its lane when awaited and rejects
// another running operation on that lane.
class websocket_client_handle final {
public:
    websocket_client_handle(const websocket_client_handle& other) noexcept;
    websocket_client_handle& operator=(const websocket_client_handle&) = delete;

    [[nodiscard]] websocket_client_handle with_options(operation_options options) const;
    [[nodiscard]] task<std::optional<websocket_message>> read() const;
    [[nodiscard]] task<void> text(std::string_view payload, websocket_send_options options = {}) const;
    template <std::same_as<std::pmr::string> string_type>
    [[nodiscard]] task<void> text(string_type&& payload, websocket_send_options options = {}) const {
        return text_owned(std::move(payload), options);
    }
    [[nodiscard]] task<void> binary(std::string_view payload, websocket_send_options options = {}) const;
    [[nodiscard]] task<void> binary(std::span<const std::byte> payload, websocket_send_options options = {}) const;
    template <std::same_as<std::pmr::string> string_type>
    [[nodiscard]] task<void> binary(string_type&& payload, websocket_send_options options = {}) const {
        return binary_owned(std::move(payload), options);
    }
    [[nodiscard]] task<void> ping(std::string_view payload = {}) const;
    template <std::same_as<std::pmr::string> string_type>
    [[nodiscard]] task<void> ping(string_type&& payload) const {
        return ping_owned(std::move(payload));
    }
    [[nodiscard]] task<void> pong(std::string_view payload) const;
    template <std::same_as<std::pmr::string> string_type>
    [[nodiscard]] task<void> pong(string_type&& payload) const {
        return pong_owned(std::move(payload));
    }
    [[nodiscard]] task<void> close(websocket_close_options options = {}) const;
    void abort() noexcept;

private:
    friend class detail::websocket_client_state;
    websocket_client_handle(detail::websocket_client_state& state_value,
        ::ruvia::operation_scope& scope, operation_options options) noexcept;
    static void expire_capability(void* target) noexcept;
    [[nodiscard]] task<void> text_owned(std::pmr::string&& payload, websocket_send_options options) const;
    [[nodiscard]] task<void> binary_owned(std::pmr::string&& payload, websocket_send_options options) const;
    [[nodiscard]] task<void> ping_owned(std::pmr::string&& payload) const;
    [[nodiscard]] task<void> pong_owned(std::pmr::string&& payload) const;

    detail::websocket_client_state* state_;
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
    // one, so these APIs are lvalue-only. The client connects once: a completed
    // close handshake, a transport or protocol failure, a timeout, or
    // cancellation of any operation retires it.
    [[nodiscard]] task<void> connect() &;
    task<void> connect() && = delete;
    [[nodiscard]] websocket_client_handle with_options(operation_options options) const&;
    websocket_client_handle with_options(operation_options) const&& = delete;
    // Returns the default-options handle. Each call registers one allocation-free
    // capability with the client operation scope, so keep the returned handle for
    // a sequence of operations instead of calling handle() per operation.
    [[nodiscard]] websocket_client_handle handle() const&;
    websocket_client_handle handle() const&& = delete;

    // Idempotent and callable from any thread. This only requests immediate
    // transport termination; over HTTP/2 a running session first writes
    // GOAWAY(NO_ERROR) (RFC 9113 section 6.8), waiting at most 1 s (product
    // limit), as on every retirement. Graceful RFC 6455 close uses the handle:
    // co_await client.handle().close({...}). Use shutdown() when completion must
    // be awaited.
    void close() noexcept;
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
