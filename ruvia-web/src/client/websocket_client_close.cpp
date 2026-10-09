#include <array>
#include <exception>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "ruvia/core/asio_task.h"

#include "client/websocket_client_internal.h"
#include "client/websocket_client_state.h"

namespace ruvia::detail {

void websocket_client_state::close_on_worker(abort_reason_type reason) noexcept {
    if (!worker_.is_current()) {
        std::terminate();
    }
    const auto previous = phase_.exchange(phase_type::closed, std::memory_order_acq_rel);
    if (previous == phase_type::closed) {
        return;
    }
    if (abort_reason_ == abort_reason_type::none) {
        abort_reason_ = reason;
    }
    stop_source_.request_stop();
    if (http2_) {
        http2_->stop();
    }
    if (http3_) {
        http3_->stop();
    }
    resolver_.cancel();
    disarm(connect_timer_);
    disarm(read_timer_);
    disarm(write_timer_);
    disarm(heartbeat_timer_);
    disarm(close_handshake_timer_);
    write_signal_.notify();
    liveness_state_ = websocket_liveness_idle{};
    if (protocol_) {
        (void)protocol_->abort();
    }
    std::error_code ignored;
    (void)stream_.lowest_layer().cancel(ignored);
    (void)stream_.lowest_layer().close(ignored);
    if (http2_ || http3_) {
        start_close_on_worker();
    }
}

void websocket_client_state::request_abort(abort_reason_type reason) noexcept {
    auto phase = phase_.load(std::memory_order_acquire);
    if (phase == phase_type::closed) {
        return;
    }
    if (phase == phase_type::fresh && phase_.compare_exchange_strong(phase, phase_type::closed,
                                          std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
    }
    if (worker_.is_current()) {
        close_on_worker(reason);
        return;
    }
    try {
        auto state_value = shared_from_this();
        if (!loop_.defer_cleanup(
                [state_value = std::move(state_value), reason] { state_value->close_on_worker(reason); }) &&
            phase_.load(std::memory_order_acquire) != phase_type::closed) {
            std::terminate();
        }
    } catch (...) {
        if (phase_.load(std::memory_order_acquire) != phase_type::closed) {
            std::terminate();
        }
    }
}

void websocket_client_state::abort() noexcept {
    request_abort(abort_reason_type::closing);
}

void websocket_client_state::request_cancel() noexcept {
    request_abort(abort_reason_type::cancelled);
}

task<void> websocket_client_state::shutdown_owned(
    std::shared_ptr<websocket_client_state> state_value, client_close_state::observation_mode_type mode) {
    auto* owner_value = state_value.get();
    return owner_value->close_state_.shutdown_owned(std::move(state_value), [owner_value] { owner_value->start_close_on_worker(); }, mode);
}

void websocket_client_state::start_close_on_worker() noexcept {
    if (!worker_.is_current()) {
        std::terminate();
    }
    close_on_worker(abort_reason_type::closing);
    stop_source_.request_stop();
    close_state_.start_cleanup(shared_from_this(), [this] { return close_on_worker(); }, [this](std::exception_ptr failure) { finish_close(std::move(failure)); });
}

task<void> websocket_client_state::close_on_worker() {
    while (connect_in_flight_ || heartbeat_in_flight_) {
        co_await close_state_.wait();
    }
    co_await operation_scope_.close_and_join();
    if (http2_) {
        co_await http2_->join();
    }
    if (http3_) {
        co_await http3_->join();
    }
}

void websocket_client_state::finish_close(std::exception_ptr failure) {
    if (connect_in_flight_ || heartbeat_in_flight_ || operation_scope_.has_pending_operations()) {
        std::terminate();
    }
    phase_.store(phase_type::closed, std::memory_order_release);
    close_state_.finish(std::move(failure));
}

task<void> websocket_client_state::shutdown() {
    return shutdown_owned(shared_from_this(), client_close_state::observation_mode_type::caller);
}

scoped_operation<void> websocket_client_state::close(
    websocket_close_options options, operation_options operation_options_value) {
    validate_operation_options(operation_options_value);
    require_current();
    std::pmr::string reason(options.reason_.view(), memory_.resource());
    auto read_activity = claim_activity(read_active_, "WebSocket client close cannot overlap read");
    auto write_activity = claim_activity(write_active_, "WebSocket client close cannot overlap write");
    auto close_activity = claim_activity(close_active_, "WebSocket client close is already in progress");
    return ::ruvia::make_scoped_operation(operation_scope_,
        close_owned(shared_from_this(), options, std::move(reason), std::move(operation_options_value),
            std::move(read_activity), std::move(write_activity), std::move(close_activity)),
        &websocket_client_state::check_operation_affinity, &worker_);
}

task<void> websocket_client_state::close_owned(std::shared_ptr<websocket_client_state> state_value,
    websocket_close_options options, std::pmr::string reason, operation_options operation_options_value,
    operation_lane_lease read_activity, operation_lane_lease write_activity, operation_lane_lease close_activity) {
    static_cast<void>(read_activity);
    static_cast<void>(write_activity);
    static_cast<void>(close_activity);
    state_value->require_open();
    operation_guard_type operation(*state_value, operation_options_value);
    {
        co_await state_value->wait_for_write_idle();
        state_value->require_open();
        write_guard_type write_guard(*state_value, write_phase_type::application);
        const auto submitted = state_value->require_protocol().submit_close(options.code_, reason);
        if (submitted != websocket_close_submit_status::accepted) {
            throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                "invalid WebSocket client close payload");
        }
        state_value->phase_.store(phase_type::closing, std::memory_order_release);
        co_await state_value->flush_output();
    }
    // The close-handshake limit starts after the local Close frame is committed.
    // Keep it on its own timer so peer traffic and control-frame responses cannot
    // restart the deadline for the next transport read.
    state_value->arm(state_value->close_handshake_timer_, state_value->config_.close_handshake_timeout_,
        abort_reason_type::timeout);
    std::array<char, websocket_client_close_handshake_buffer_bytes> bytes_value{};
    for (;;) {
        std::optional<websocket_event> event;
        {
            co_await state_value->wait_for_write_idle();
            write_guard_type write_guard(*state_value, write_phase_type::application);
            event = state_value->require_protocol().next_event();
            if (event.has_value() && event->ping() != nullptr) {
                co_await state_value->flush_output();
            }
        }
        if (!event.has_value()) {
            const auto count = co_await state_value->read_transport(bytes_value, std::nullopt);
            if (count == 0) {
                state_value->require_protocol().notify_transport_eof();
                state_value->close_on_worker(abort_reason_type::none);
                throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                    "WebSocket transport ended before peer Close");
            }
            (void)state_value->require_protocol().feed(std::string_view(bytes_value.data(), count));
            continue;
        }
        if (event->protocol_error() != nullptr) {
            co_await websocket_client_state::throw_protocol_error_after_flush(state_value,
                "WebSocket peer violated the protocol during close handshake");
            std::terminate();
        }
        if (event->close() != nullptr || event->transport_end() != nullptr) {
            co_await state_value->wait_for_write_idle();
            write_guard_type write_guard(*state_value, write_phase_type::application);
            co_await state_value->flush_output();
            if (state_value->phase_.load(std::memory_order_acquire) != phase_type::closed) {
                state_value->close_on_worker(abort_reason_type::none);
            }
            co_return;
        }
    }
}

}  // namespace ruvia::detail
