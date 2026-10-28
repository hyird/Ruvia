#include <exception>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/operation_timeout.h"

#include "client/websocket_client_state.h"
#include "util/pmr_owned_storage.h"

namespace ruvia::detail {

void websocket_client_state::finish_write(write_phase_type phase) noexcept {
    if (write_phase_ != phase) {
        std::terminate();
    }
    write_phase_ = write_phase_type::idle;
    write_signal_.notify();
    resume_pong_deadline();
}

task<void> websocket_client_state::wait_for_write_idle(write_wait_type type) {
    const bool reader = type == write_wait_type::reader;
    try {
        for (;;) {
            throw_if_aborted();
            if (write_phase_ == write_phase_type::idle && (reader || !reader_waiting_)) {
                if (reader) {
                    // The caller claims the lane before any writer resumes.
                    reader_waiting_ = false;
                }
                co_return;
            }
            if (reader) {
                reader_waiting_ = true;
            }
            co_await write_signal_.wait();
        }
    } catch (...) {
        if (reader) {
            // Writers held back for this reader re-check the idle lane.
            reader_waiting_ = false;
            write_signal_.notify();
        }
        throw;
    }
}

task<void> websocket_client_state::write_transport(std::string_view bytes_value) {
    if (http2_) {
        worker_timer_registration timer;
        arm(timer, config_.write_timeout_, client_abort_reason::timeout);
        co_await http2_->write(bytes_value);
        disarm(timer);
        throw_if_aborted();
        touch_activity();
        co_return;
    }

    if (bytes_value.empty()) {
        co_return;
    }
    // The transport write lane owns the deadline. Its timeout result retires
    // the whole connection like any other transport failure.
    const operation_timeout timeout(config_.write_timeout_);
    const auto result_value = co_await transport_.write(bytes_value, timeout);
    if (!result_value.ok()) {
        throw_transport_failure(result_value);
    }
    throw_if_aborted();
    touch_activity();
}

task<void> websocket_client_state::flush_output() {
    for (;;) {
        auto& protocol = require_protocol();
        const auto plan = protocol.output_plan();
        if (!plan.bytes().empty()) {
            co_await write_transport(plan.bytes());
            if (protocol.consume_output(plan.bytes().size()) == websocket_output_consume_status::out_of_range) {
                std::terminate();
            }
            continue;
        }
        if (plan.disposition() == websocket_transport_disposition::end_transport) {
            // The stream end waits for the peer's half; a local close already
            // bounds it, a peer-initiated close starts the same guard here.
            if (!close_handshake_timer_.registered()) {
                arm(close_handshake_timer_, config_.close_handshake_timeout_, client_abort_reason::timeout);
            }
            if (http2_) {
                co_await http2_->finish();
            }
            protocol.commit_transport_end();
            retire(client_abort_reason::closing);
        }
        co_return;
    }
}

task<void> websocket_client_state::throw_protocol_error_after_flush(std::string_view message) {
    co_await wait_for_write_idle(write_wait_type::reader);
    try {
        write_guard_type write_guard(*this, write_phase_type::application);
        co_await flush_output();
    } catch (const websocket_client_error& error) {
        if (error.code() != websocket_client_error::code_type::io_error &&
            error.code() != websocket_client_error::code_type::tls_failed) {
            throw;
        }
    }
    fail_transport();
    throw websocket_client_error(websocket_client_error::code_type::protocol_error, message);
}

task<void> websocket_client_state::write(websocket_opcode opcode, std::string_view payload_value,
    const operation_options& options, websocket_send_options send_options) {
    require_current();
    std::pmr::string owned(payload_value, memory_.resource());
    return write(opcode, std::move(owned), options, send_options);
}

task<void> websocket_client_state::write(websocket_opcode opcode, std::pmr::string&& payload_value,
    const operation_options& options, websocket_send_options send_options) {
    require_current();
    auto owned = rebind_owned_storage(payload_value, memory_.resource());
    return ::ruvia::make_scoped_operation(lifecycle_.operation_scope(),
        run_write(opcode, std::move(owned), options, send_options),
        &websocket_client_state::check_operation_affinity, &worker_);
}

task<void> websocket_client_state::run_write(websocket_opcode opcode, std::pmr::string payload_value,
    operation_options options, websocket_send_options send_options) {
    lifecycle_.require_ready();
    auto activity = claim_activity(write_active_, "concurrent WebSocket client writes are not supported");
    operation_guard_type operation(*this, options);
    co_await wait_for_write_idle(write_wait_type::writer);
    lifecycle_.require_ready();
    write_guard_type write_guard(*this, write_phase_type::application);
    const auto submitted = require_protocol().submit_frame(opcode, payload_value, send_options.compress_);
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
    co_await flush_output();
}

}  // namespace ruvia::detail
