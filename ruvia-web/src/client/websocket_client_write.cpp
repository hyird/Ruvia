#include <chrono>
#include <exception>
#include <optional>
#include <string_view>
#include <utility>

#include <asio/buffer.hpp>
#include <asio/write.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/stop_token.h"

#include "client/websocket_client_internal.h"
#include "client/websocket_client_state.h"

namespace ruvia::detail {

void websocket_client_state::finish_write(write_phase_type phase) noexcept {
    if (write_phase_ != phase) {
        std::terminate();
    }
    write_phase_ = write_phase_type::idle;
    write_signal_.notify();
}

task<void> websocket_client_state::wait_for_write_idle() {
    for (;;) {
        throw_abort();
        if (write_phase_ == write_phase_type::idle) {
            co_return;
        }
        co_await write_signal_.wait();
    }
}

task<void> websocket_client_state::write_transport(
    std::string_view bytes_value, std::optional<std::chrono::milliseconds> configured_timeout) {
    if (bytes_value.empty()) {
        co_return;
    }
    arm(write_timer_, configured_timeout, abort_reason_type::timeout);
    try {
        if (http3_) {
            co_await http3_->write(bytes_value);
        } else if (http2_) {
            co_await http2_->write(bytes_value);
        } else {
            co_await write_socket(bytes_value);
        }
        disarm(write_timer_);
        throw_abort();
    } catch (...) {
        disarm(write_timer_);
        throw;
    }
}

task<void> websocket_client_state::write_socket(std::string_view bytes_value) {
    auto initiate_write = [this, bytes_value](auto handler) {
        if (config_.scheme_ == websocket_scheme::wss) {
            asio::async_write(stream_, asio::buffer(bytes_value), std::move(handler));
        } else {
            asio::async_write(stream_.next_layer(), asio::buffer(bytes_value), std::move(handler));
        }
    };
    const auto completion = co_await ruvia::async_asio<std::size_t>(std::move(initiate_write));
    throw_abort();
    if (completion.error_code()) {
        throw websocket_client_error(
            websocket_client_transport_error_code(config_.scheme_ == websocket_scheme::wss),
            completion.error_code().message());
    }
    touch_activity();
}

task<void> websocket_client_state::flush_output() {
    for (;;) {
        auto& protocol = require_protocol();
        const auto plan = protocol.output_plan();
        if (!plan.bytes().empty()) {
            co_await write_transport(plan.bytes(), config_.write_timeout_);
            if (protocol.consume_output(plan.bytes().size()) == websocket_output_consume_status::out_of_range) {
                std::terminate();
            }
            continue;
        }
        if (plan.disposition() == websocket_transport_disposition::end_transport) {
            if (http3_) {
                co_await http3_->finish();
            } else if (http2_) {
                co_await http2_->finish();
            }
            protocol.commit_transport_end();
            close_on_worker(abort_reason_type::none);
        }
        co_return;
    }
}

task<void> websocket_client_state::throw_protocol_error_after_flush(
    std::shared_ptr<websocket_client_state> state_value, std::string_view message) {
    co_await state_value->wait_for_write_idle();
    try {
        write_guard_type write_guard(*state_value, write_phase_type::application);
        co_await state_value->flush_output();
    } catch (const websocket_client_error& error) {
        if (error.code() != websocket_client_error::code_type::io_error &&
            error.code() != websocket_client_error::code_type::tls_failed) {
            throw;
        }
    }
    if (state_value->phase_.load(std::memory_order_acquire) != phase_type::closed) {
        state_value->close_on_worker(abort_reason_type::none);
    }
    throw websocket_client_error(websocket_client_error::code_type::protocol_error, message);
}

scoped_operation<void> websocket_client_state::write(
    websocket_opcode opcode, std::string_view payload_value, operation_options options, websocket_send_options send_options) {
    validate_operation_options(options);
    require_current();
    std::pmr::string owned(payload_value, memory_.resource());
    return ::ruvia::make_scoped_operation(operation_scope_,
        write_owned(shared_from_this(), opcode, std::move(owned), std::move(options), send_options,
            claim_activity(write_active_, "concurrent WebSocket client writes are not supported")),
        &websocket_client_state::check_operation_affinity, &worker_);
}

task<void> websocket_client_state::write_owned(std::shared_ptr<websocket_client_state> state_value,
    websocket_opcode opcode, std::pmr::string payload_value, operation_options options, websocket_send_options send_options,
    operation_lane_lease activity) {
    static_cast<void>(activity);
    state_value->require_open();
    operation_guard_type operation(*state_value, options);
    co_await state_value->wait_for_write_idle();
    state_value->require_open();
    write_guard_type write_guard(*state_value, write_phase_type::application);
    const auto submitted = state_value->require_protocol().submit_frame(opcode, payload_value, send_options.compress_);
    switch (submitted) {
        case websocket_frame_submit_status::accepted:
            break;
        case websocket_frame_submit_status::message_too_large:
            throw websocket_client_error(websocket_client_error::code_type::message_too_large,
                "WebSocket client message exceeds configured limit");
        case websocket_frame_submit_status::not_open:
            throw websocket_client_error(
                websocket_client_error::code_type::closing, "WebSocket client is closing");
        default:
            throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                "invalid WebSocket client frame payload");
    }
    co_await state_value->flush_output();
}

}  // namespace ruvia::detail
