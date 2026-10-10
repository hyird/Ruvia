#include <array>
#include <chrono>
#include <exception>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include <asio/buffer.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/stop_token.h"

#include "client/websocket_client_internal.h"
#include "client/websocket_client_state.h"
#include "tls/tls_stream_end.h"
#include "websocket/http_websocket_liveness.h"

namespace ruvia::detail {

scoped_operation<std::optional<websocket_message>> websocket_client_state::read(
    operation_options options) {
    validate_operation_options(options);
    require_current();
    return ::ruvia::make_scoped_operation(operation_scope_,
        read_owned(shared_from_this(), std::move(options),
            claim_activity(read_active_, "concurrent WebSocket client reads are not supported")),
        &websocket_client_state::check_operation_affinity, &worker_);
}

task<std::optional<websocket_message>> websocket_client_state::read_owned(
    std::shared_ptr<websocket_client_state> state_value, operation_options options, operation_lane_lease activity) {
    static_cast<void>(activity);
    state_value->require_open();
    operation_guard_type operation(*state_value, options);
    std::array<char, websocket_client_transport_buffer_bytes> bytes_value{};
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
            const auto count = co_await state_value->read_transport(
                bytes_value, state_value->config_.read_timeout_);
            if (count == 0) {
                state_value->require_protocol().notify_transport_eof();
                state_value->close_on_worker(abort_reason_type::none);
                throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                    "WebSocket transport ended before peer Close");
            }
            state_value->feed_input(std::string_view(bytes_value.data(), count));
            continue;
        }
        if (const auto* message = event->message()) {
            co_return websocket_message::borrow(message->opcode(), message->payload());
        }
        if (const auto* pong = event->pong()) {
            if (websocket_heartbeat_pong_matches(state_value->liveness_state_, pong->payload())) {
                state_value->liveness_state_ = websocket_liveness_idle{};
                state_value->touch_activity();
            }
            continue;
        }
        if (event->protocol_error() != nullptr) {
            co_await websocket_client_state::throw_protocol_error_after_flush(
                state_value, "WebSocket peer violated the protocol");
            std::terminate();
        }
        if (event->close() != nullptr || event->transport_end() != nullptr) {
            co_await state_value->wait_for_write_idle();
            write_guard_type write_guard(*state_value, write_phase_type::application);
            co_await state_value->flush_output();
            co_return std::nullopt;
        }
    }
}

task<std::size_t> websocket_client_state::read_transport(
    std::span<char> output, std::optional<std::chrono::milliseconds> configured_timeout) {
    arm(read_timer_, configured_timeout, abort_reason_type::timeout);
    try {
        const auto count = http3_ ? co_await http3_->read(output) : (http2_ ? co_await http2_->read(output) : co_await read_socket(output));
        disarm(read_timer_);
        throw_abort();
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
        close_on_worker(abort_reason_type::none);
        throw websocket_client_error(websocket_client_error::code_type::message_too_large,
            "WebSocket client input exceeds the buffered frame limit");
    }
}

task<std::size_t> websocket_client_state::read_socket(std::span<char> output) {
    auto initiate_read = [this, output](auto handler) {
        if (config_.scheme_ == websocket_scheme::wss) {
            stream_.async_read_some(asio::buffer(output.data(), output.size()), std::move(handler));
        } else {
            stream_.next_layer().async_read_some(
                asio::buffer(output.data(), output.size()), std::move(handler));
        }
    };
    const auto completion = co_await ruvia::async_asio<std::size_t>(std::move(initiate_read));
    throw_abort();
    if (is_stream_read_end(completion.error_code())) {
        co_return 0;
    }
    if (completion.error_code()) {
        throw websocket_client_error(
            websocket_client_transport_error_code(config_.scheme_ == websocket_scheme::wss),
            completion.error_code().message());
    }
    touch_activity();
    co_return completion.result();
}

}  // namespace ruvia::detail
