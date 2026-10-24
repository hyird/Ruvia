#include <exception>
#include <optional>
#include <utility>

#include "client/websocket_client_state.h"

namespace ruvia::detail {

void websocket_client_state::retire(client_abort_reason reason) noexcept {
    if (!worker_.is_current()) {
        std::terminate();
    }
    // The first reason wins: it is recorded before the lifecycle requests stop,
    // so cancellation callbacks of in-flight operations cannot replace it.
    if (abort_reason_ == client_abort_reason::none) {
        abort_reason_ = reason;
    }
    lifecycle_.request_close();
}

void websocket_client_state::request_retire(client_abort_reason reason) noexcept {
    if (worker_.is_current()) {
        retire(reason);
        return;
    }
    bool deferred = false;
    try {
        deferred = loop_.defer_cleanup([state = shared_from_this(), reason] { state->retire(reason); });
    } catch (...) {
        deferred = false;
    }
    if (!deferred) {
        // The loop is retiring and already owns the shutdown of this client,
        // or defer_cleanup failed and request_close() enforces the terminal guard.
        lifecycle_.request_close();
    }
}

void websocket_client_state::fail_transport() noexcept {
    retire(client_abort_reason::closing);
}

void websocket_client_state::abort() noexcept {
    request_retire(client_abort_reason::closing);
}

void websocket_client_state::close_now() noexcept {
    if (abort_reason_ == client_abort_reason::none) {
        abort_reason_ = client_abort_reason::closing;
    }
    if (http2_) {
        http2_->stop();
    }
    transport_.abort(abort_reason_);
    transport_.close();
    disarm(connect_timer_);
    disarm(heartbeat_timer_);
    disarm(close_handshake_timer_);
    write_signal_.notify();
    liveness_state_ = websocket_liveness_idle{};
    if (protocol_) {
        (void)protocol_->abort();
    }
}

task<void> websocket_client_state::join() {
    co_await heartbeat_tasks_.join();
    co_await join_transports();
}

task<void> websocket_client_state::join_transports() {
    // Each protocol driver is joined once, either by a failed connect or by
    // the lifecycle's retirement, and dropped afterwards.
    if (http2_) {
        co_await http2_->join();
        http2_.reset();
    }
}

task<void> websocket_client_state::close(
    websocket_close_options options, const operation_options& operation_options_value) {
    require_current();
    std::pmr::string reason(options.reason_.view(), memory_.resource());
    return ::ruvia::make_scoped_operation(lifecycle_.operation_scope(),
        run_close(options, std::move(reason), operation_options_value),
        &websocket_client_state::check_operation_affinity, &worker_);
}

task<void> websocket_client_state::run_close(websocket_close_options options, std::pmr::string reason,
    operation_options operation_options_value) {
    lifecycle_.require_ready();
    auto read_activity = claim_activity(read_active_, "WebSocket client close cannot overlap read");
    auto write_activity = claim_activity(write_active_, "WebSocket client close cannot overlap write");
    auto close_activity = claim_activity(close_active_, "WebSocket client close is already in progress");
    operation_guard_type operation(*this, operation_options_value);
    {
        co_await wait_for_write_idle(write_wait_type::writer);
        lifecycle_.require_ready();
        write_guard_type write_guard(*this, write_phase_type::application);
        const auto submitted = require_protocol().submit_close(options.code_, reason);
        if (submitted != websocket_close_submit_status::accepted) {
            throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                "invalid WebSocket client close payload");
        }
        co_await flush_output();
    }
    // The close-handshake limit starts after the local Close frame is committed.
    // Keep it on its own timer so peer traffic and control-frame responses cannot
    // restart the deadline for the next transport read.
    arm(close_handshake_timer_, config_.close_handshake_timeout_, client_abort_reason::timeout);
    for (;;) {
        const auto event = co_await poll_protocol_event(
            std::nullopt, "WebSocket peer violated the protocol during close handshake");
        if (event.close() != nullptr || event.transport_end() != nullptr) {
            // The close handshake is complete: once the server has ended the
            // transport (or failed to in time), retire the connection.
            co_await await_peer_transport_end();
            retire(client_abort_reason::closing);
            co_return;
        }
    }
}

}  // namespace ruvia::detail
