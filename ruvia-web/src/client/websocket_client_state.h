#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include <asio/ssl/context.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/websocket_connection.h"
#include "ruvia/web/websocket_client.h"

#include "client/client_lifecycle.h"
#include "client/client_tcp_transport.h"
#include "client/websocket_client_config_storage.h"
#include "http2/websocket_http2_transport.h"
#include "util/operation_lane_lease.h"
#include "websocket/http_websocket_liveness.h"

namespace ruvia::detail {

// One WebSocket connection over HTTP/1.1 or HTTP/2, retired through the
// shared client_lifecycle. The state is its own lifecycle backend: connect()
// establishes the transport, performs the opening handshake and starts the
// protocol; close_now() stops every I/O source; join() retires the heartbeat
// and protocol drivers. Operations borrow `this`: the lifecycle joins them, and
// close_now() cancels every timer, before the state can be retired.
class websocket_client_state final : public std::enable_shared_from_this<websocket_client_state> {
public:
    websocket_client_state(event_loop loop, const websocket_client_config& config);

    websocket_client_state(const websocket_client_state&) = delete;
    websocket_client_state& operator=(const websocket_client_state&) = delete;

    [[nodiscard]] client_lifecycle<websocket_client_state>& lifecycle() noexcept {
        return lifecycle_;
    }
    [[nodiscard]] websocket_client_handle handle(operation_options options);
    // Any thread: retires the client with the closing reason on its worker.
    void abort() noexcept;
    [[nodiscard]] bool connected();
    [[nodiscard]] std::string_view subprotocol();
    [[nodiscard]] const worker_handle& worker() const noexcept {
        return worker_;
    }

    [[nodiscard]] scoped_operation<std::optional<websocket_message>> read(const operation_options& options);
    // Owns payload bytes in the client resource before returning a cold task.
    [[nodiscard]] scoped_operation<void> write(websocket_opcode opcode, std::string_view payload,
        const operation_options& options, websocket_send_options send_options = {});
    [[nodiscard]] scoped_operation<void> close(
        websocket_close_options options, const operation_options& operation_options);

private:
    friend class client_lifecycle<websocket_client_state>;
    friend class websocket_http2_transport;

    static constexpr std::string_view client_name{"WebSocket"};

    enum class write_phase_type : std::uint8_t { idle,
        application,
        heartbeat };

    enum class write_wait_type : std::uint8_t { writer,
        reader };

    class write_guard_type final {
    public:
        write_guard_type(websocket_client_state& state_value, write_phase_type phase)
            : state_(state_value),
              phase_(phase) {
            if (state_.write_phase_ != write_phase_type::idle) {
                throw std::logic_error("concurrent WebSocket client writes are not supported");
            }
            state_.write_phase_ = phase_;
        }

        ~write_guard_type() {
            state_.finish_write(phase_);
        }

        write_guard_type(const write_guard_type&) = delete;
        write_guard_type& operator=(const write_guard_type&) = delete;

    private:
        websocket_client_state& state_;
        write_phase_type phase_;
    };

    [[nodiscard]] static operation_lane_lease claim_activity(bool& active, const char* message) {
        operation_lane_lease lease(active);
        if (!lease) {
            throw websocket_client_error(websocket_client_error::code_type::invalid_state, message);
        }
        return lease;
    }

    // Bounds one application operation by its timeout and stop token; either
    // retires the whole connection with the matching abort reason.
    class operation_guard_type final {
    public:
        operation_guard_type(websocket_client_state& state_value, const operation_options& options);
        ~operation_guard_type();

        operation_guard_type(const operation_guard_type&) = delete;
        operation_guard_type& operator=(const operation_guard_type&) = delete;

    private:
        websocket_client_state& state_;
        worker_timer_registration timer_;
        stop_registration cancellation_;
    };

    // client_lifecycle backend.
    [[nodiscard]] websocket_client_state& backend() noexcept {
        return *this;
    }
    [[noreturn]] static void throw_not_ready();
    [[nodiscard]] task<void> connect();
    void close_now() noexcept;
    [[nodiscard]] task<void> join();

    [[nodiscard]] task<void> establish();
    [[nodiscard]] task<void> join_transports();
    [[nodiscard]] task<void> perform_handshake();

    [[nodiscard]] task<std::optional<websocket_message>> run_read(operation_options options);
    [[nodiscard]] task<void> run_write(websocket_opcode opcode, std::pmr::string payload,
        operation_options options, websocket_send_options send_options);
    [[nodiscard]] task<void> run_close(websocket_close_options options, std::pmr::string reason,
        operation_options operation_options);

    void require_current() const;
    [[nodiscard]] ruvia::websocket_connection& require_protocol() noexcept;
    [[nodiscard]] bool protocol_open() const noexcept;
    // Worker only: records the first abort reason and retires the client.
    void retire(client_abort_reason reason) noexcept;
    // Any thread, including stop callbacks: defers retire(reason) to the worker
    // so a lifecycle-initiated close records its own reason first.
    void request_retire(client_abort_reason reason) noexcept;
    // A transport-internal fatal failure: the client closes.
    void fail_transport() noexcept;
    [[nodiscard]] static websocket_client_error abort_error(client_abort_reason reason);
    void throw_if_aborted() const;
    [[nodiscard]] websocket_client_error transport_error(const client_transport_result& result) const;
    // Retires the client and throws the error describing a failed transport result.
    [[noreturn]] void throw_transport_failure(const client_transport_result& result);
    void finish_write(write_phase_type phase) noexcept;
    // A waiting reader goes first: a writer that finishes and writes again
    // would otherwise retake the lane before the reader's deferred wake-up.
    [[nodiscard]] task<void> wait_for_write_idle(write_wait_type type);
    [[nodiscard]] task<void> heartbeat();
    void heartbeat_timer_fired() noexcept;
    [[nodiscard]] bool heartbeat_ping_due() noexcept;
    void arm_heartbeat_timer(std::chrono::milliseconds delay);
    void touch_activity() noexcept;
    // Restarts an expired Pong window when a reader resumes polling input.
    void resume_pong_deadline() noexcept;
    [[nodiscard]] std::chrono::milliseconds heartbeat_delay(std::int64_t now) const noexcept;
    [[nodiscard]] task<void> flush_output();
    [[nodiscard]] task<void> throw_protocol_error_after_flush(std::string_view message);
    [[nodiscard]] task<websocket_event> poll_protocol_event(
        std::optional<std::chrono::milliseconds> read_timeout, std::string_view protocol_error_message);
    [[nodiscard]] task<std::size_t> read_transport(std::span<char> output,
        std::optional<std::chrono::milliseconds> configured_timeout);
    void feed_input(std::string_view bytes);
    [[nodiscard]] task<void> write_transport(std::string_view bytes);
    void arm(worker_timer_registration& timer, std::optional<std::chrono::milliseconds> timeout,
        client_abort_reason reason);
    void disarm(worker_timer_registration& timer) noexcept;
    [[nodiscard]] static bool generate_mask(void*, websocket_mask_key_type& key) noexcept;
    static void check_operation_affinity(void* target) noexcept;

    event_loop loop_;
    worker_handle worker_;
    worker_memory memory_;
    websocket_client_config_storage config_;
    asio::ssl::context tls_context_;
    client_wire_counters wire_counters_{};
    client_tcp_transport transport_;
    worker_timer_registration connect_timer_;
    worker_timer_registration heartbeat_timer_;
    worker_timer_registration close_handshake_timer_;
    worker_signal write_signal_;
    // Opening-handshake response bytes, then the transport read buffer of the
    // single input reader (read_active_).
    std::pmr::string input_;
    std::optional<ruvia::websocket_connection> protocol_;
    std::optional<websocket_http2_transport> http2_;
    std::pmr::string selected_subprotocol_;
    websocket_compression negotiated_compression_{};
    client_abort_reason abort_reason_{client_abort_reason::none};
    bool read_active_{false};
    bool write_active_{false};
    bool close_active_{false};
    write_phase_type write_phase_{write_phase_type::idle};
    bool reader_waiting_{false};
    websocket_liveness_state_type liveness_state_{websocket_liveness_idle{}};
    std::uint64_t heartbeat_sequence_{0};
    std::int64_t last_active_ms_{0};
    bool heartbeat_in_flight_{false};
    task_scope heartbeat_tasks_;
    // Last: retires operations before the backend and its allocator.
    client_lifecycle<websocket_client_state> lifecycle_;
};

}  // namespace ruvia::detail
