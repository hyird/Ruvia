#include <array>
#include <chrono>
#include <exception>
#include <optional>
#include <span>
#include <string_view>

#include "ruvia/core/operation_timeout.h"

#include "client/websocket_client_internal.h"
#include "client/websocket_client_state.h"
#include "websocket/http_websocket_liveness.h"

namespace ruvia::detail {

scoped_operation<std::optional<websocket_message>> websocket_client_state::read(
    const operation_options& options) {
    require_current();
    return ::ruvia::make_scoped_operation(lifecycle_.operation_scope(),
        run_read(options),
        &websocket_client_state::check_operation_affinity, &worker_);
}

task<std::optional<websocket_message>> websocket_client_state::run_read(operation_options options) {
    lifecycle_.require_ready();
    auto activity = claim_activity(read_active_, "concurrent WebSocket client reads are not supported");
    operation_guard_type operation(*this, options);
    std::array<char, websocket_client_transport_buffer_bytes> bytes_value{};
    for (;;) {
        std::optional<websocket_event> event;
        {
            co_await wait_for_write_idle();
            write_guard_type write_guard(*this, write_phase_type::application);
            event = require_protocol().next_event();
            if (event.has_value() && event->ping() != nullptr) {
                co_await flush_output();
            }
        }
        if (!event.has_value()) {
            const auto count = co_await read_transport(bytes_value, config_.read_timeout_);
            if (count == 0) {
                require_protocol().notify_transport_eof();
                fail_transport();
                throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                    "WebSocket transport ended before peer Close");
            }
            feed_input(std::string_view(bytes_value.data(), count));
            continue;
        }
        if (const auto* message = event->message()) {
            co_return websocket_message::borrow(message->opcode(), message->payload());
        }
        if (const auto* pong = event->pong()) {
            if (websocket_heartbeat_pong_matches(liveness_state_, pong->payload())) {
                liveness_state_ = websocket_liveness_idle{};
                touch_activity();
            }
            continue;
        }
        if (event->protocol_error() != nullptr) {
            co_await throw_protocol_error_after_flush("WebSocket peer violated the protocol");
            std::terminate();
        }
        if (event->close() != nullptr || event->transport_end() != nullptr) {
            co_await wait_for_write_idle();
            write_guard_type write_guard(*this, write_phase_type::application);
            co_await flush_output();
            co_return std::nullopt;
        }
    }
}

task<std::size_t> websocket_client_state::read_transport(
    std::span<char> output, std::optional<std::chrono::milliseconds> configured_timeout) {
    // One deadline mechanism for every protocol: the timer retires the whole
    // connection, which aborts the transport with the timeout reason.
    arm(read_timer_, configured_timeout, client_abort_reason::timeout);
    try {
        std::size_t count = 0;
        const operation_timeout unbounded(std::nullopt);
        const auto result_value = co_await transport_.read_some(output, unbounded);
        if (result_value.status_ != client_transport_status::eof) {
            if (!result_value.ok()) {
                throw_transport_failure(result_value);
            }
            count = result_value.bytes_;
        }
        disarm(read_timer_);
        throw_if_aborted();
        if (count != 0) {
            touch_activity();
        }
        co_return count;
    } catch (...) {
        disarm(read_timer_);
        throw;
    }
}

void websocket_client_state::feed_input(std::string_view bytes_value) {
    // A rejected feed consumes nothing. Dropping it would splice later bytes
    // into the buffered frame, so an exhausted input bound terminates instead.
    if (require_protocol().feed(bytes_value) == websocket_feed_status::backpressured) {
        fail_transport();
        throw websocket_client_error(websocket_client_error::code_type::message_too_large,
            "WebSocket client input exceeds the buffered frame limit");
    }
}

}  // namespace ruvia::detail
