#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/task.h"

#include "http3/http3_body_budget.h"
#include "http3/http3_client_receive_driver.h"
#include "http3/http3_client_sans_io_session_engine.h"
#include "http3/http3_quic_client_endpoint_resolver.h"
#include "http3/http3_quic_client_socket_session.h"

namespace ruvia::detail {

// RFC 9114 section 8: the connection error that a protocol failure reports.
[[nodiscard]] inline std::uint64_t http3_client_protocol_close_code(
    const http3_client_sans_io_session_result& result) noexcept {
    return static_cast<std::uint64_t>(result.code_ == http3_connection_error_code::no_error
                                          ? http3_connection_error_code::general_protocol_error
                                          : result.code_);
}

// The sole HTTP/3 client session chain, shared by the HTTP client connection
// and the WebSocket transport: DNS, endpoint attempts sharing one connect
// deadline, the QUIC pump/wait loop, critical streams (written by the socket
// session straight from the engine's pending output), peer-stream accept and
// QPACK-blocked input buffering. Each endpoint attempt starts from a fresh
// HTTP/3 engine. The owner keeps request/stream semantics and supplies these
// worker-affine hooks; the driver is its friend:
//   std::optional<time_point_type> next_deadline(std::optional<time_point_type> bound) const noexcept;
//   void expire(time_point_type now);            // owner deadlines before the handshake completes
//   bool active() const noexcept;                // owner work keeps the session open
//   bool draining() const noexcept;              // no new work; close once inactive
//   void attempt_started(http3_client_sans_io_session_engine&);  // before the socket session exists
//   void attempt_closing();                      // the attempt's session is still live
//   void attempt_abandoned();                    // session closed; engine still live
//   bool early_stream_rejected(std::uint64_t stream_id);  // 0-RTT rejected request stream
//   bool sweep(bool requests_may_start, bool early_data_only);  // per-tick protocol work
//   std::size_t peer_stream_read_budget(std::uint64_t stream_id);  // 0 skips this tick
//   bool peer_stream_input(std::uint64_t stream_id, const http3_client_receive_driver::result_type&);
// sweep() calls receive_peer_streams(owner) at its chosen point. Owner hooks
// may throw; fail() records the outcome and RFC 9114 close code first. The
// task running run() must complete before close()/reset_attempt() and before
// destruction; all members are owner-worker affine.
class http3_client_session_driver final {
public:
    using clock_type = std::chrono::steady_clock;
    using time_point_type = clock_type::time_point;

    enum class outcome_type : std::uint8_t {
        cancelled,
        deadline,
        resolve_failed,
        connect_failed,
        transport_error,
        protocol_error,
        response_too_large,
    };
    struct config_type final {
        // Unbracketed transport host for DNS.
        std::string_view host_{};
        std::uint16_t port_{};
        // SNI, verified peer name and session-ticket key.
        std::string_view tls_host_{};
        clock_type::duration connect_timeout_{};
        // Unset keeps an inactive ready session open until stop.
        std::optional<clock_type::duration> idle_timeout_{};
        // Inactivity bound for pending critical-stream output.
        std::optional<clock_type::duration> write_timeout_{};
        http3_client_sans_io_response_limits limits_{};
        ruvia::quic_version initial_version_{ruvia::quic_version::v1};
        bool enable_early_data_{};
    };

    // host_/tls_host_, tls, resource and body_budget must outlive this driver.
    // A null body_budget selects each engine's own budget.
    http3_client_session_driver(asio::io_context& io, http3_quic_client_tls_context& tls,
        const config_type& config, std::pmr::memory_resource* resource, http3_body_budget* body_budget = nullptr);
    ~http3_client_session_driver();
    http3_client_session_driver(const http3_client_session_driver&) = delete;
    http3_client_session_driver& operator=(const http3_client_session_driver&) = delete;

    // Resolves, attempts endpoints and pumps until the session ends. Never
    // throws; the attempt (if any) stays open for close().
    template <typename owner_type>
    [[nodiscard]] task<outcome_type> run(owner_type& owner);
    // Closes the remaining attempt's QUIC connection with the recorded close
    // code within one bounded closing period, then stops its engine.
    [[nodiscard]] task<void> close();
    // Releases the closed attempt after the owner retired its stream state.
    void reset_attempt() noexcept;
    void request_stop() noexcept;
    // Wakes the pump, or re-arms an in-progress DNS wait.
    void wake() noexcept;
    [[noreturn]] void fail(outcome_type outcome, std::optional<std::uint64_t> close_code, const char* message);

    // Accepts new peer unidirectional streams and reads each fairly through
    // the receive driver, which buffers QPACK-blocked input.
    template <typename owner_type>
    [[nodiscard]] bool receive_peer_streams(owner_type& owner);
    // Forgets a peer stream whose QUIC delivery the owner already stopped.
    void retire_peer_stream(std::uint64_t stream_id) noexcept;

    [[nodiscard]] bool stopping() const noexcept {
        return stopping_;
    }
    [[nodiscard]] outcome_type terminal_failure() const noexcept {
        return terminal_failure_;
    }
    // The exception that ended run(), if any.
    [[nodiscard]] std::exception_ptr failure() const noexcept {
        return failure_;
    }
    [[nodiscard]] bool has_attempt() const noexcept {
        return attempt_ != nullptr;
    }
    // Requires an attempt.
    [[nodiscard]] http3_client_sans_io_session_engine& engine();
    [[nodiscard]] const http3_client_sans_io_session_engine& engine() const;
    [[nodiscard]] http3_client_receive_driver& receiver();
    // Null between attempts and once the attempt's QUIC session is closed.
    [[nodiscard]] http3_quic_client_socket_session* session() const noexcept {
        return attempt_ ? attempt_->session_.get() : nullptr;
    }
    // Nonzero and incremented by every endpoint attempt.
    [[nodiscard]] std::uint64_t generation() const noexcept {
        return generation_;
    }

private:
    using session_owner_type = std::unique_ptr<http3_quic_client_socket_session,
        pmr_object_deleter<http3_quic_client_socket_session>>;
    struct attempt_type final {
        attempt_type(std::pmr::memory_resource* resource, http3_body_budget* body_budget,
            const http3_client_sans_io_response_limits& limits);
        attempt_type(const attempt_type&) = delete;
        attempt_type& operator=(const attempt_type&) = delete;

        http3_client_sans_io_session_engine engine_;
        http3_client_receive_driver receiver_;
        std::pmr::vector<std::uint64_t> peer_streams_;
        // Declared last: destroyed before the engine it drives.
        session_owner_type session_;
    };
    using attempt_owner_type = std::unique_ptr<attempt_type, pmr_object_deleter<attempt_type>>;
    // Bounds the RFC 9000 closing period (three PTOs) that follows each
    // CONNECTION_CLOSE, so a slow or lossy peer cannot hold teardown open.
    static constexpr clock_type::duration max_closing_period = std::chrono::seconds(1);

    template <typename owner_type>
    [[nodiscard]] task<bool> drive_endpoint(owner_type& owner, const asio::ip::udp::endpoint& peer,
        time_point_type connect_deadline);
    template <typename owner_type>
    [[nodiscard]] bool replay_rejected_early_streams(owner_type& owner);
    [[nodiscard]] static time_point_type deadline_after(time_point_type now, clock_type::duration timeout) noexcept;
    [[nodiscard]] static clock_type::duration endpoint_budget(
        time_point_type now, time_point_type connect_deadline, std::size_t remaining_endpoints) noexcept;
    void begin_attempt();
    void open_session(const asio::ip::udp::endpoint& peer);
    [[nodiscard]] task<void> close_attempt(time_point_type latest);
    void abandon_run(bool stopping) noexcept;
    void track_critical_output(time_point_type now, bool progress);
    void accept_peer_streams();
    void close_peer_stream(std::size_t index);
    [[nodiscard]] task<bool> yield();

    asio::io_context& io_;
    http3_quic_client_tls_context& tls_;
    config_type config_;
    std::pmr::memory_resource* resource_;
    http3_body_budget* body_budget_;
    http3_quic_client_endpoint_resolver resolver_;
    attempt_owner_type attempt_;
    std::optional<time_point_type> critical_deadline_{};
    std::exception_ptr failure_{};
    std::uint64_t generation_{};
    outcome_type terminal_failure_{outcome_type::transport_error};
    // RFC 9114 code for the closing CONNECTION_CLOSE; unset means H3_NO_ERROR.
    std::optional<std::uint64_t> close_error_code_{};
    bool stopping_{};
};

template <typename owner_type>
task<http3_client_session_driver::outcome_type> http3_client_session_driver::run(owner_type& owner) {
    using status_type = http3_quic_client_endpoint_resolver::status_type;
    outcome_type failure = outcome_type::connect_failed;
    try {
        const auto connect_deadline = deadline_after(clock_type::now(), config_.connect_timeout_);
        std::optional<http3_quic_client_endpoint_resolver::result_type> resolved;
        while (!stopping_) {
            auto attempt_value = co_await resolver_.resolve(config_.host_, config_.port_, owner.next_deadline(connect_deadline));
            const auto now = clock_type::now();
            owner.expire(now);
            if (!owner.active()) {
                break;
            }
            if (now >= connect_deadline ||
                (attempt_value.status_ != status_type::interrupted && attempt_value.status_ != status_type::timeout)) {
                resolved.emplace(std::move(attempt_value));
                break;
            }
        }
        if (stopping_ || (resolved && resolved->status_ == status_type::stopped)) {
            failure = outcome_type::cancelled;
        } else if (!resolved || resolved->status_ == status_type::timeout || clock_type::now() >= connect_deadline) {
            failure = outcome_type::deadline;
        } else if (resolved->status_ != status_type::resolved) {
            failure = outcome_type::resolve_failed;
        } else {
            const auto& endpoints = resolved->endpoints_;
            for (std::size_t index = 0; index < endpoints.size(); ++index) {
                const auto now = clock_type::now();
                if (stopping_ || now >= connect_deadline) {
                    failure = stopping_ ? outcome_type::cancelled : outcome_type::deadline;
                    break;
                }
                const bool try_next = co_await drive_endpoint(owner, endpoints[index],
                    now + endpoint_budget(now, connect_deadline, endpoints.size() - index));
                if (!try_next || stopping_) {
                    failure = stopping_ ? outcome_type::cancelled : outcome_type::transport_error;
                    break;
                }
                if (!owner.active() || index + 1 == endpoints.size()) {
                    break;
                }
                // This attempt never completed its handshake. Close it within
                // the connect budget, then let the next endpoint start from
                // fresh protocol state with re-armed owner streams.
                owner.attempt_closing();
                co_await close_attempt(std::min(connect_deadline, deadline_after(clock_type::now(), max_closing_period)));
                owner.attempt_abandoned();
                attempt_.reset();
            }
            if (!stopping_ && clock_type::now() >= connect_deadline && failure == outcome_type::connect_failed) {
                failure = outcome_type::deadline;
            }
        }
    } catch (...) {
        failure_ = std::current_exception();
        failure = stopping_ ? outcome_type::cancelled : terminal_failure_;
        abandon_run(stopping_);
    }
    co_return failure;
}

template <typename owner_type>
task<bool> http3_client_session_driver::drive_endpoint(
    owner_type& owner, const asio::ip::udp::endpoint& peer, time_point_type connect_deadline) {
    using session_type = http3_quic_client_socket_session;
    begin_attempt();
    owner.attempt_started(attempt_->engine_);
    open_session(peer);
    auto& engine = attempt_->engine_;
    auto& session = *attempt_->session_;
    bool had_http3 = false;
    std::optional<time_point_type> idle_deadline;
    std::size_t consecutive_work_ticks = 0;
    while (!stopping_) {
        (void)session.consume_work_notification();
        const auto tick = session.pump();
        if (tick.protocol_failure_) {
            fail(outcome_type::protocol_error, http3_client_protocol_close_code(*tick.protocol_failure_),
                "HTTP/3 remembered settings were rejected");
        }
        const bool rejected_early_streams = replay_rejected_early_streams(owner);
        if (tick.status_ == session_type::pump_status_type::fatal || tick.status_ == session_type::pump_status_type::closed) {
            break;
        }
        const bool ready = session.transport().info().state_ == ruvia::quic_connection_state::ready;
        had_http3 |= ready;
        const auto now = clock_type::now();
        if (!ready) {
            owner.expire(now);
            if (now >= connect_deadline) {
                break;
            }
        }
        const bool early = !ready && session.early_data_enabled() &&
                           session.transport().info().early_data_ == ruvia::quic_early_data_state::available;
        const bool protocol_progress = rejected_early_streams ||
                                       ((ready || early) && owner.sweep(tick.critical_streams_ready_, early));
        if (ready) {
            track_critical_output(now, tick.critical_output_progress_);
        }
        if (engine.peer_settings()) {
            session.remember_resumption_ticket(engine.peer_settings());
        }
        const bool active = owner.active();
        if (active || !ready || owner.draining() || !config_.idle_timeout_) {
            idle_deadline.reset();
        } else if (!idle_deadline) {
            idle_deadline = deadline_after(now, *config_.idle_timeout_);
        }
        if (!active && (owner.draining() || !ready)) {
            break;
        }
        if (idle_deadline && now >= *idle_deadline) {
            break;
        }
        if (tick.received_ || tick.sent_ || tick.critical_output_progress_ || protocol_progress) {
            if (++consecutive_work_ticks < 8) {
                continue;  // Drain only a bounded amount before yielding to the worker.
            }
            consecutive_work_ticks = 0;
            if (!co_await yield()) {
                break;
            }
            continue;
        }
        consecutive_work_ticks = 0;
        auto deadline_value = owner.next_deadline(ready ? std::nullopt : std::optional{connect_deadline});
        for (const auto& candidate : {idle_deadline, critical_deadline_}) {
            if (candidate && (!deadline_value || *candidate < *deadline_value)) {
                deadline_value = candidate;
            }
        }
        const auto wake = co_await session.wait_for_activity(tick, deadline_value);
        if (wake == session_type::wake_reason_type::stopped || wake == session_type::wake_reason_type::fatal ||
            (wake == session_type::wake_reason_type::deadline && !ready && clock_type::now() >= connect_deadline)) {
            break;
        }
        // Owner work can race the timer callback on this worker. Re-evaluate
        // it at the top of the loop before committing idle retirement.
    }
    co_return !had_http3 && !stopping_;
}

template <typename owner_type>
bool http3_client_session_driver::replay_rejected_early_streams(owner_type& owner) {
    auto& session = *attempt_->session_;
    std::array<std::uint64_t, 32> rejected{};
    bool progress_value = false;
    while (true) {
        const auto count = session.take_rejected_early_streams(rejected);
        for (std::size_t index = 0; index < count; ++index) {
            progress_value |= owner.early_stream_rejected(rejected[index]);
        }
        if (count < rejected.size()) {
            return progress_value;
        }
    }
}

template <typename owner_type>
bool http3_client_session_driver::receive_peer_streams(owner_type& owner) {
    using status_type = http3_client_receive_driver::status_type;
    auto& attempt = *attempt_;
    auto& quic = attempt.session_->transport();
    const auto listed_before = attempt.peer_streams_.size();
    accept_peer_streams();
    bool progress_value = attempt.peer_streams_.size() != listed_before;
    auto& peer_streams = attempt.peer_streams_;
    const auto still_listed = [&peer_streams](std::size_t index, std::uint64_t id) noexcept {
        return index < peer_streams.size() && peer_streams[index] == id;
    };
    for (std::size_t index = 0; index < peer_streams.size();) {
        const auto id = peer_streams[index];
        const auto read_budget = owner.peer_stream_read_budget(id);
        if (!still_listed(index, id)) {
            progress_value = true;
            continue;
        }
        if (read_budget == 0) {
            ++index;
            continue;
        }
        const auto health = quic.read_health(id);
        const auto part = health.status_ == ruvia::quic_stream_read_status::reset
                              ? attempt.receiver_.accept_reset(id, health.peer_reset_error_code_)
                              : attempt.receiver_.drive(id, [&quic](std::uint64_t stream_id, std::span<char> bytes_value) { return quic.read_stream(stream_id, std::as_writable_bytes(bytes_value)); }, read_budget);
        if (part.status_ == status_type::connection_error) {
            fail(outcome_type::protocol_error, http3_client_protocol_close_code(part.protocol_), "HTTP/3 peer stream failed");
        }
        if (part.status_ == status_type::transport_error) {
            fail(outcome_type::transport_error, std::nullopt, "HTTP/3 peer stream transport failed");
        }
        if (owner.peer_stream_input(id, part)) {
            progress_value = true;
            if (still_listed(index, id)) {
                ++index;
            }
            continue;
        }
        switch (part.status_) {
            case status_type::peer_stream_ended:
            case status_type::stream_reset:
            case status_type::stream_error:
                close_peer_stream(index);
                progress_value = true;
                break;
            case status_type::response_complete:
                throw std::runtime_error("HTTP/3 peer stream completed a message without an owner");
            default:
                progress_value |= part.status_ == status_type::progress;
                ++index;
                break;
        }
    }
    return progress_value;
}

}  // namespace ruvia::detail
