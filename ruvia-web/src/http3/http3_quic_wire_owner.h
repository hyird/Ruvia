#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <system_error>
#include <thread>

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/steady_timer.hpp>

#include "http3/http3_datagram_endpoint.h"
#include "http3/http3_quic_server_transport.h"
#include "http3/http3_quic_tls_context.h"

namespace ruvia::detail {

// The HTTP/3 worker's QUIC wire: the worker's own datagram endpoint, the
// sans-I/O QUIC server and its expiry timer. Owner-thread only. Packet ingress
// and egress live here, driven from the owner's single protocol loop; the owner
// forwards endpoint notifications (on_endpoint_stopping) and timer events wake
// that loop. The worker io_context, endpoint and TLS context must outlive it;
// the endpoint is prepared and started by the owner before construction.
class http3_quic_wire_owner final {
public:
    using clock_type = std::chrono::steady_clock;

    struct wake_type final {
        void* context_{};
        void (*wake_)(void*) noexcept {};
    };

    struct received_type final {
        enum class kind_type : std::uint8_t { none,
            dropped,
            connection,
            failed_connection,
            initial_offer };
        kind_type kind_{kind_type::none};
        ruvia::quic_connection_token connection_{};
        ruvia::quic_initial_offer offer_{};
    };

    // timer_handler_resource is borrowed through owner destruction and supplies
    // the associated allocator for timer waits and their retirement posts.
    // Builds the QUIC server without consuming endpoint input.
    http3_quic_wire_owner(asio::io_context& worker_io, http3_worker_datagram_endpoint& endpoint,
        http3_quic_tls_context& tls, ruvia::quic_server_config transport_config,
        std::pmr::memory_resource* timer_handler_resource, wake_type wake);
    ~http3_quic_wire_owner();

    http3_quic_wire_owner(const http3_quic_wire_owner&) = delete;
    http3_quic_wire_owner& operator=(const http3_quic_wire_owner&) = delete;
    http3_quic_wire_owner(http3_quic_wire_owner&&) = delete;
    http3_quic_wire_owner& operator=(http3_quic_wire_owner&&) = delete;

    // Starts consuming endpoint input. A failure is recorded and starts the stop.
    void start() noexcept;
    void poll_endpoint() noexcept;
    // The endpoint stopped or failed; the wire stops producing and cancels its timer.
    void on_endpoint_stopping() noexcept;
    // Routes one received datagram into the QUIC server and consumes it. A
    // Version Negotiation reply is sent here. kind none: nothing was received.
    [[nodiscard]] received_type receive_one(clock_type::time_point now);
    // A received datagram is waiting (the ingress batch budget was exhausted).
    [[nodiscard]] bool receive_pending() const noexcept {
        return started_ && !stopping_ && endpoint_.receive_slot().has_value();
    }
    // Writes packets of one connection until it has none or the endpoint has no
    // free output buffer. Returns the packet count, or nullopt when the
    // connection failed.
    [[nodiscard]] std::optional<std::size_t> send(ruvia::quic_connection_token token, clock_type::time_point now);
    [[nodiscard]] bool output_available() noexcept;
    // Arms the single timer for the earlier of QUIC expiry and local_deadline.
    void schedule(std::optional<clock_type::time_point> local_deadline) noexcept;
    // True once after the armed deadline elapsed.
    [[nodiscard]] bool take_expiry() noexcept;

    [[nodiscard]] http3_quic_server_transport& transport() noexcept {
        return *transport_;
    }
    [[nodiscard]] bool transport_available() const noexcept {
        return transport_.has_value() && !stopping_;
    }
    [[nodiscard]] std::uint16_t bound_port() const noexcept;
    [[nodiscard]] bool stopping() const noexcept {
        return stopping_;
    }
    [[nodiscard]] std::exception_ptr failure() const noexcept {
        return failure_;
    }

    // Stop: no new input, timer cancelled. The caller retires every QUIC
    // connection first; poll_stop then destroys the transport once timer
    // handlers and native endpoint operations have retired.
    void request_stop() noexcept;
    void poll_stop() noexcept;
    [[nodiscard]] bool stopped() const noexcept {
        return transport_destroyed_;
    }

private:
    class timer_wait_handler_type final {
    public:
        using allocator_type = std::pmr::polymorphic_allocator<std::byte>;

        timer_wait_handler_type(http3_quic_wire_owner& owner_value, std::uint64_t generation) noexcept;
        [[nodiscard]] allocator_type get_allocator() const noexcept {
            return allocator_;
        }
        timer_wait_handler_type(timer_wait_handler_type&& other) noexcept;
        ~timer_wait_handler_type();

        timer_wait_handler_type(const timer_wait_handler_type&) = delete;
        timer_wait_handler_type& operator=(const timer_wait_handler_type&) = delete;
        timer_wait_handler_type& operator=(timer_wait_handler_type&&) = delete;

        void operator()(const asio::error_code& error) noexcept;

    private:
        http3_quic_wire_owner* owner_{};
        std::uint64_t generation_{};
        allocator_type allocator_;
    };

    class timer_retirement_handler_type final {
    public:
        using allocator_type = std::pmr::polymorphic_allocator<std::byte>;

        explicit timer_retirement_handler_type(http3_quic_wire_owner& owner_value) noexcept;
        [[nodiscard]] allocator_type get_allocator() const noexcept {
            return allocator_;
        }
        timer_retirement_handler_type(timer_retirement_handler_type&& other) noexcept;
        ~timer_retirement_handler_type();

        timer_retirement_handler_type(const timer_retirement_handler_type&) = delete;
        timer_retirement_handler_type& operator=(const timer_retirement_handler_type&) = delete;
        timer_retirement_handler_type& operator=(timer_retirement_handler_type&&) = delete;

        void operator()() noexcept;

    private:
        http3_quic_wire_owner* owner_{};
        allocator_type allocator_;
    };

    void require_owner_thread() const noexcept;
    void wake() noexcept;
    [[nodiscard]] bool send_packet(std::span<std::byte> packet_buffer,
        const ruvia::quic_packet_result& packet);
    void reconcile_timer() noexcept;
    void arm_timer(clock_type::time_point deadline) noexcept;
    void on_timer_completion(std::uint64_t generation,
        const asio::error_code& error) noexcept;
    void on_timer_handler_retired(std::uint64_t generation) noexcept;
    void on_timer_retirement_wake() noexcept;
    void on_timer_retirement_handler_retired() noexcept;
    void record_failure(std::exception_ptr failure) noexcept;
    void record_current_failure() noexcept;
    void capture_endpoint_error() noexcept;

    std::thread::id owner_thread_;
    asio::io_context& network_io_;
    wake_type wake_;
    http3_worker_datagram_endpoint& endpoint_;
    asio::steady_timer timer_;
    std::pmr::polymorphic_allocator<std::byte> timer_handler_allocator_;
    std::optional<http3_quic_server_transport> transport_;
    std::optional<clock_type::time_point> desired_deadline_;
    std::optional<clock_type::time_point> armed_deadline_;
    std::exception_ptr failure_;
    std::uint64_t timer_generation_{};
    std::uint64_t armed_generation_{};
    std::uint64_t failed_timer_submission_generation_{};
    bool timer_submitting_{};
    bool started_{};
    bool stopping_{};
    bool expired_{};
    bool timer_wait_outstanding_{};
    bool timer_cancel_requested_{};
    bool timer_completion_delivered_{};
    bool timer_wake_outstanding_{};
    bool timer_handlers_retired_{true};
    bool transport_destroyed_{};
};

}  // namespace ruvia::detail
