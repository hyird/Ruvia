#include <algorithm>
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
    resume_pong_deadline();
    for (;;) {
        const auto event =
            co_await poll_protocol_event(config_.read_timeout_, "WebSocket peer violated the protocol");
        if (const auto* message = event.message()) {
            co_return websocket_message::borrow(message->opcode(), message->payload());
        }
        if (const auto* pong = event.pong()) {
            if (websocket_heartbeat_pong_matches(liveness_state_, pong->payload())) {
                liveness_state_ = websocket_liveness_idle{};
                touch_activity();
            }
            continue;
        }
        if (event.close() != nullptr || event.transport_end() != nullptr) {
            co_return std::nullopt;
        }
    }
}

task<websocket_event> websocket_client_state::poll_protocol_event(
    std::optional<std::chrono::milliseconds> read_timeout, std::string_view protocol_error_message) {
    for (;;) {
        std::optional<websocket_event> event;
        {
            co_await wait_for_write_idle(write_wait_type::reader);
            write_guard_type write_guard(*this, write_phase_type::application);
            event = require_protocol().next_event();
            if (event.has_value() && event->ping() != nullptr) {
                co_await flush_output();
            }
        }
        if (!event.has_value()) {
            const auto read_bytes = std::clamp(
                (std::max)(input_.capacity() - input_.size(), require_protocol().needed_input_bytes()),
                websocket_client_transport_buffer_bytes, std::size_t{64} * 1024);
            const auto space = input_.prepare(read_bytes);
            const auto count = co_await read_transport(space, read_timeout);
            if (count == 0) {
                require_protocol().notify_transport_eof();
                fail_transport();
                throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                    "WebSocket transport ended before peer Close");
            }
            input_.commit(count);
            continue;
        }
        if (event->ping() != nullptr) {
            continue;
        }
        if (event->protocol_error() != nullptr) {
            co_await throw_protocol_error_after_flush(protocol_error_message);
            std::terminate();
        }
        if (event->close() != nullptr || event->transport_end() != nullptr) {
            co_await wait_for_write_idle(write_wait_type::reader);
            write_guard_type write_guard(*this, write_phase_type::application);
            co_await flush_output();
        }
        co_return *event;
    }
}

task<std::size_t> websocket_client_state::read_transport(
    std::span<char> output, std::optional<std::chrono::milliseconds> configured_timeout) {
    if (http2_) {
        worker_timer_registration timer;
        arm(timer, configured_timeout, client_abort_reason::timeout);
        const auto count = co_await http2_->read(output);
        disarm(timer);
        throw_if_aborted();
        if (count != 0) {
            touch_activity();
        }
        co_return count;
    }

    // The transport read lane owns the deadline. Its timeout result retires
    // the whole connection like any other transport failure.
    const operation_timeout timeout(configured_timeout);
    const auto result_value = co_await transport_.read_some(output, timeout);
    std::size_t count = 0;
    if (result_value.status_ != client_transport_status::eof) {
        if (!result_value.ok()) {
            throw_transport_failure(result_value);
        }
        count = result_value.bytes_;
    }
    throw_if_aborted();
    if (count != 0) {
        touch_activity();
    }
    co_return count;
}

}  // namespace ruvia::detail
