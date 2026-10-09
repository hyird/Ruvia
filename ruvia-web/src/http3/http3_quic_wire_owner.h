#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/steady_timer.hpp>

#include "http3/http3_datagram_endpoint.h"
#include "http3/http3_quic_server_transport.h"
#include "http3/http3_quic_tls_context.h"

namespace ruvia::detail {

// Owner-thread-only QUIC wire driver borrowing the Acceptor's datagram channel.
// The worker io_context, channel and TLS context must outlive it.
class http3_quic_wire_owner final {
public:
    using clock_type = std::chrono::steady_clock;

    enum class protocol_pump_result_type : std::uint8_t { idle,
        progress,
        fatal };

    struct protocol_pump_type final {
        void* context_{};
        protocol_pump_result_type (*drive_)(void*, http3_quic_server_transport&,
            http3_worker_datagram_endpoint&) noexcept {};
    };

    struct stop_status_type final {
        bool stopping_{};
        bool endpoint_retired_{};
        bool timer_handlers_retired_{};
        bool transport_destroyed_{};
        bool outbound_pending_{};
        bool failed_{};

        [[nodiscard]] bool complete() const noexcept {
            return stopping_ && endpoint_retired_ && timer_handlers_retired_ && transport_destroyed_ &&
                   !outbound_pending_;
        }
    };

    // timer_handler_resource is borrowed through owner destruction and supplies
    // the associated allocator for timer waits and their retirement posts.
    http3_quic_wire_owner(asio::io_context& worker_io, http3_datagram_channel& channel,
        http3_worker_datagram_endpoint::udp::endpoint local_endpoint, http3_quic_tls_context& tls,
        ruvia::quic_server_config transport_config,
        std::pmr::memory_resource* timer_handler_resource, protocol_pump_type protocol_pump);
    ~http3_quic_wire_owner();

    http3_quic_wire_owner(const http3_quic_wire_owner&) = delete;
    http3_quic_wire_owner& operator=(const http3_quic_wire_owner&) = delete;
    http3_quic_wire_owner(http3_quic_wire_owner&&) = delete;
    http3_quic_wire_owner& operator=(http3_quic_wire_owner&&) = delete;

    // Prepare the worker endpoint and QUIC server without consuming channel input or
    // scheduling timers. Called once on the owner thread before the application's
    // serving barrier is released.
    // Failure starts shutdown and is rethrown; poll_stop() must complete before
    // destruction, just as after a failed start().
    void prepare();
    // Start receiving after prepare(). Synchronous failures are recorded and
    // rethrown after initiating shutdown; the owner must remain alive until
    // poll_stop() reports complete.
    void start();
    void request_stop() noexcept;
    // The worker runtime uses this gate when it must retire connections/channels
    // after a fatal endpoint stop but before destroying the QUIC transport.
    void defer_transport_retirement() noexcept;
    void release_transport_retirement() noexcept;
    // Requests another bounded owner-thread turn after a worker/channel wake.
    void request_drive() noexcept;
    void poll_datagrams() noexcept;

    // Call on the owner thread outside I/O callbacks to perform the final ordered
    // destruction once channel loans and timer completion handlers have retired.
    void poll_stop() noexcept;
    [[nodiscard]] stop_status_type stop_status() const noexcept;
    [[nodiscard]] std::uint16_t bound_port() const noexcept;
    [[nodiscard]] http3_quic_server_transport* transport() noexcept {
        return transport_ ? &*transport_ : nullptr;
    }
    [[nodiscard]] bool outbound_quiescent() const noexcept {
        return endpoint_.outbound_quiescent();
    }
    [[nodiscard]] bool endpoint_io_quiescent() const noexcept {
        require_owner_thread();
        return endpoint_.endpoint_retired() && !endpoint_.outbound_pending();
    }
    [[nodiscard]] bool consume_transport_activity() noexcept {
        require_owner_thread();
        return std::exchange(transport_activity_, false);
    }
    [[nodiscard]] std::size_t timer_expirations() const noexcept;
    [[nodiscard]] std::exception_ptr failure() const noexcept;
    void rethrow_failure() const;

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

    static void endpoint_notification(void* context,
        http3_worker_datagram_endpoint::notification_kind kind) noexcept;

    void require_owner_thread() const noexcept;
    void on_endpoint_notification(http3_worker_datagram_endpoint::notification_kind kind) noexcept;
    void drive() noexcept;
    void update_deadline();
    void reconcile_timer() noexcept;
    void arm_timer(clock_type::time_point deadline) noexcept;
    void on_timer_completion(std::uint64_t generation,
        const asio::error_code& error) noexcept;
    void on_timer_handler_retired(std::uint64_t generation) noexcept;
    void on_timer_retirement_wake() noexcept;
    void on_timer_retirement_handler_retired() noexcept;
    void begin_stop() noexcept;
    void record_failure(std::exception_ptr failure) noexcept;
    void record_current_failure() noexcept;
    void record_error(std::error_code error, const char* operation) noexcept;
    void capture_endpoint_error() noexcept;

    std::thread::id owner_thread_;
    asio::io_context& network_io_;
    http3_quic_tls_context& tls_;
    ruvia::quic_server_config transport_config_;
    http3_worker_datagram_endpoint endpoint_;
    asio::steady_timer timer_;
    std::pmr::polymorphic_allocator<std::byte> timer_handler_allocator_;
    protocol_pump_type protocol_pump_{};
    std::optional<http3_quic_server_transport> transport_;
    std::optional<clock_type::time_point> desired_deadline_;
    std::optional<clock_type::time_point> armed_deadline_;
    std::exception_ptr failure_;
    std::uint64_t timer_generation_{};
    std::uint64_t armed_generation_{};
    std::uint64_t failed_timer_submission_generation_{};
    std::size_t timer_expirations_{};
    bool timer_submitting_{};
    bool prepared_{};
    bool started_{};
    bool stopping_{};
    bool driving_{};
    bool drive_requested_{};
    bool timer_wait_outstanding_{};
    bool timer_cancel_requested_{};
    bool timer_completion_delivered_{};
    bool timer_wake_outstanding_{};
    bool timer_handlers_retired_{true};
    bool transport_destroyed_{true};
    bool transport_retirement_released_{true};
    bool transport_activity_{};
};

}  // namespace ruvia::detail
