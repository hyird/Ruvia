#pragma once

#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/websocket_connection.h"
#include "ruvia/web/websocket_client.h"

#include "client/client_close_state.h"
#include "client/websocket_client_config_storage.h"
#include "http2/websocket_http2_transport.h"
#include "http3/websocket_http3_transport.h"
#include "util/operation_lane_lease.h"
#include "websocket/http_websocket_liveness.h"

namespace ruvia::detail {

class websocket_client_state final : public std::enable_shared_from_this<websocket_client_state> {
public:
    websocket_client_state(event_loop loop, const websocket_client_config& config);
    ~websocket_client_state();

    void bind_stop();
    [[nodiscard]] task<void> connect();
    [[nodiscard]] task<void> shutdown();
    [[nodiscard]] websocket_client_handle handle(operation_options options);
    void abort() noexcept;
    void request_cancel() noexcept;
    [[nodiscard]] bool connected();
    [[nodiscard]] std::string_view subprotocol();
    [[nodiscard]] const worker_handle& worker() const noexcept {
        return worker_;
    }

    [[nodiscard]] scoped_operation<std::optional<websocket_message>> read(operation_options options);
    [[nodiscard]] scoped_operation<void> write(
        websocket_opcode opcode, std::string_view payload, operation_options options, websocket_send_options send_options = {});
    [[nodiscard]] scoped_operation<void> close(
        websocket_close_options options, operation_options operation_options);

private:
    friend class websocket_http2_transport;
    friend class websocket_http3_transport;
    enum class phase_type : std::uint8_t { fresh,
        connecting,
        open,
        closing,
        closed };
    enum class abort_reason_type : std::uint8_t { none,
        timeout,
        cancelled,
        closing };
    enum class write_phase_type : std::uint8_t { idle,
        application,
        heartbeat };
    enum class write_claim_type : std::uint8_t { acquire,
        adopt };

    class write_guard_type final {
    public:
        write_guard_type(
            websocket_client_state& state_value, write_phase_type phase, write_claim_type claim = write_claim_type::acquire)
            : state_(state_value),
              phase_(phase) {
            if (phase_ == write_phase_type::idle) {
                std::terminate();
            }
            if (claim == write_claim_type::acquire) {
                if (state_.write_phase_ != write_phase_type::idle) {
                    throw std::logic_error("concurrent WebSocket client writes are not supported");
                }
                state_.write_phase_ = phase_;
            } else if (state_.write_phase_ != phase_) {
                std::terminate();
            }
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

    [[nodiscard]] static task<void> connect_owned(std::shared_ptr<websocket_client_state> state);
    [[nodiscard]] static task<void> shutdown_owned(std::shared_ptr<websocket_client_state> state,
        client_close_state::observation_mode_type mode);
    [[nodiscard]] static task<std::optional<websocket_message>> read_owned(
        std::shared_ptr<websocket_client_state> state, operation_options options,
        operation_lane_lease activity);
    [[nodiscard]] static task<void> write_owned(std::shared_ptr<websocket_client_state> state,
        websocket_opcode opcode, std::pmr::string payload, operation_options options, websocket_send_options send_options,
        operation_lane_lease activity);
    [[nodiscard]] static task<void> close_owned(std::shared_ptr<websocket_client_state> state,
        websocket_close_options options, std::pmr::string reason, operation_options operation_options,
        operation_lane_lease read_activity, operation_lane_lease write_activity, operation_lane_lease close_activity);

    void require_current() const;
    void require_open() const;
    [[nodiscard]] ruvia::websocket_connection& require_protocol() noexcept;
    [[nodiscard]] std::uint16_t port() const noexcept;
    void close_on_worker(abort_reason_type reason) noexcept;
    void start_close_on_worker() noexcept;
    [[nodiscard]] task<void> close_on_worker();
    void finish_close(std::exception_ptr failure);
    void request_abort(abort_reason_type reason) noexcept;
    [[nodiscard]] task<void> establish_transport();
    [[nodiscard]] task<void> perform_tls_handshake();
    void finish_write(write_phase_type phase) noexcept;
    [[nodiscard]] task<void> wait_for_write_idle();
    [[nodiscard]] static task<void> heartbeat_owned(std::shared_ptr<websocket_client_state> state);
    void finish_heartbeat() noexcept;
    void heartbeat_timer_fired() noexcept;
    void arm_heartbeat_timer(std::chrono::milliseconds delay);
    void touch_activity() noexcept;
    [[nodiscard]] std::chrono::milliseconds heartbeat_delay(std::int64_t now) const noexcept;
    [[nodiscard]] task<void> flush_output();
    [[nodiscard]] static task<void> throw_protocol_error_after_flush(
        std::shared_ptr<websocket_client_state> state, std::string_view message);
    [[nodiscard]] task<std::size_t> read_transport(std::span<char> output,
        std::optional<std::chrono::milliseconds> configured_timeout);
    [[nodiscard]] task<void> write_transport(std::string_view bytes,
        std::optional<std::chrono::milliseconds> configured_timeout);
    [[nodiscard]] task<void> perform_handshake();
    [[nodiscard]] task<std::size_t> read_socket(std::span<char> output);
    [[nodiscard]] task<void> write_socket(std::string_view bytes);
    void arm(worker_timer_registration& timer, std::optional<std::chrono::milliseconds> timeout,
        abort_reason_type reason);
    void disarm(worker_timer_registration& timer) noexcept;
    void throw_abort() const;
    [[nodiscard]] static bool generate_mask(void*, websocket_mask_key_type& key) noexcept;
    static void check_operation_affinity(void* target) noexcept;

    event_loop loop_;
    worker_handle worker_;
    worker_memory memory_;
    websocket_client_config_storage config_;
    asio::ssl::context tls_context_;
    asio::ip::tcp::resolver resolver_;
    asio::ssl::stream<asio::ip::tcp::socket> stream_;
    worker_timer_registration connect_timer_;
    worker_timer_registration read_timer_;
    worker_timer_registration write_timer_;
    worker_timer_registration heartbeat_timer_;
    worker_timer_registration close_handshake_timer_;
    worker_signal write_signal_;
    client_close_state close_state_;
    std::pmr::string input_;
    std::optional<ruvia::websocket_connection> protocol_;
    std::optional<websocket_http2_transport> http2_;
    std::optional<websocket_http3_transport> http3_;
    std::pmr::string selected_subprotocol_;
    websocket_compression negotiated_compression_{};
    stop_source stop_source_;
    event_loop_stop_registration stop_registration_;
    std::atomic<phase_type> phase_{phase_type::fresh};
    abort_reason_type abort_reason_{abort_reason_type::none};
    bool read_active_{false};
    bool write_active_{false};
    bool close_active_{false};
    write_phase_type write_phase_{write_phase_type::idle};
    websocket_liveness_state_type liveness_state_{websocket_liveness_idle{}};
    std::uint64_t heartbeat_sequence_{0};
    std::int64_t last_active_ms_{0};
    bool connect_in_flight_{false};
    bool heartbeat_in_flight_{false};
    ::ruvia::operation_scope operation_scope_;
};

}  // namespace ruvia::detail
