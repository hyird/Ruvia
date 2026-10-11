#include "http3/websocket_http3_transport.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/http/websocket_client_negotiation.h"

#include "client/websocket_client_state.h"

namespace ruvia::detail {
namespace {
constexpr std::size_t receive_capacity = 64 * 1024;

http3_client_session_driver::config_type session_config(const websocket_client_config_storage& config) {
    return {.host_ = config.host_,
        .port_ = config.port_,
        .tls_host_ = config.tls_.tls_host(config.host_),
        .connect_timeout_ = std::chrono::duration_cast<http3_client_session_driver::clock_type::duration>(config.connect_timeout_),
        .write_timeout_ = config.write_timeout_
                              ? std::optional(std::chrono::duration_cast<http3_client_session_driver::clock_type::duration>(*config.write_timeout_))
                              : std::nullopt,
        .limits_ = {.max_live_streams_ = 1,
            .connection_ = {.max_active_streams_ = 1,
                .max_peer_unidirectional_streams_ = ruvia::quic_limits{}.max_streams_,
                .qpack_max_table_capacity_ = config.qpack_.max_table_capacity_,
                .qpack_blocked_streams_ = config.qpack_.max_blocked_streams_}}};
}

websocket_client_error::code_type error_code(http3_client_session_driver::outcome_type outcome) noexcept {
    using outcome_type = http3_client_session_driver::outcome_type;
    switch (outcome) {
        case outcome_type::cancelled:
            return websocket_client_error::code_type::cancelled;
        case outcome_type::deadline:
            return websocket_client_error::code_type::timeout;
        case outcome_type::resolve_failed:
            return websocket_client_error::code_type::resolve_failed;
        case outcome_type::connect_failed:
            return websocket_client_error::code_type::connect_failed;
        case outcome_type::protocol_error:
        case outcome_type::response_too_large:
            return websocket_client_error::code_type::protocol_error;
        case outcome_type::transport_error:
            return websocket_client_error::code_type::io_error;
    }
    return websocket_client_error::code_type::io_error;
}
}  // namespace

websocket_http3_transport::websocket_http3_transport(websocket_client_state& owner_value,
    const worker_handle& worker_value, std::pmr::memory_resource* resource)
    : owner_(owner_value),
      resource_(resource),
      tls_(owner_value.config_.tls_, resource),
      driver_(owner_value.loop_.io_context(), tls_, session_config(owner_value.config_), resource),
      drivers_(worker_value, {.resource_ = resource}),
      progress_(worker_value),
      received_(resource) {}

void websocket_http3_transport::check_failure() const {
    if (failure_) {
        std::rethrow_exception(failure_);
    }
    owner_.throw_if_aborted();
}

task<void> websocket_http3_transport::connect() {
    const auto& config = owner_.config_;
    drivers_.spawn(drive());
    while (!driver_.has_attempt() || !driver_.engine().peer_settings()) {
        check_failure();
        co_await progress_.wait();
    }
    check_failure();
    const auto& peer = *driver_.engine().peer_settings();
    if (!peer.enable_connect_protocol_) {
        throw websocket_client_error(websocket_client_error::code_type::handshake_rejected,
            "upstream did not enable HTTP/3 Extended CONNECT");
    }
    write_offset_ = 0;
    open_requested_ = true;
    driver_.wake();
    while (!response_) {
        check_failure();
        if (eof_) {
            throw websocket_client_error(websocket_client_error::code_type::handshake_rejected,
                "upstream closed before HTTP/3 WebSocket response");
        }
        co_await progress_.wait();
    }
    check_failure();
    const auto negotiated = config.negotiation_.validate_response(*response_, !eof_);
    if (negotiated.index() != 0) {
        throw websocket_client_error(websocket_client_error::code_type::handshake_rejected,
            "invalid HTTP/3 WebSocket handshake response");
    }
    owner_.selected_subprotocol_.assign(std::get<0>(negotiated).selected_subprotocol_);
    owner_.negotiated_compression_ = std::get<0>(negotiated).compression_;
    response_.reset();
    co_await wait_for_output();
}

void websocket_http3_transport::stop() noexcept {
    stopped_ = true;
    driver_.request_stop();
    drivers_.request_stop();
    progress_.notify();
}

task<void> websocket_http3_transport::join() {
    co_await drivers_.join();
    head_.reset();
    response_.reset();
    std::pmr::string(resource_).swap(received_);
    read_offset_ = 0;
}

void websocket_http3_transport::on_event(void* context_value, const http3_connection_event& event) {
    auto& self = *static_cast<websocket_http3_transport*>(context_value);
    switch (event.kind_) {
        case http3_connection_event_kind::final_head:
            if (self.response_ || event.head_ == nullptr) {
                throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                    "unexpected HTTP/3 WebSocket response head");
            }
            // Sink-mode delivery hands the decoded head to this owner.
            self.response_.emplace(std::move(*event.head_));
            break;
        case http3_connection_event_kind::tunnel_data:
            if (event.body_.size() > receive_capacity - self.received_.size()) {
                throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                    "HTTP/3 WebSocket receive buffer exceeded");
            }
            self.received_.append(event.body_.data(), event.body_.size());
            break;
        case http3_connection_event_kind::body:
            if (!event.body_.empty()) {
                throw websocket_client_error(websocket_client_error::code_type::handshake_rejected,
                    "upstream rejected HTTP/3 WebSocket handshake");
            }
            break;
        default:
            break;
    }
}

bool websocket_http3_transport::sweep(bool requests_may_start, bool) {
    bool progress_value = driver_.receive_peer_streams(*this);
    if (stream_id_) {
        progress_value |= receive();
    }
    if (requests_may_start) {
        progress_value |= drive_output();
    }
    if (finished_ && eof_ && !completed_) {
        // Both halves ended: our FIN is in a packet and the peer's FIN was read.
        const auto retired = driver_.session()->transport().retire_completed_stream(*stream_id_);
        if (retired == ruvia::quic_operation_status::accepted) {
            completed_ = true;
            progress_value = true;
        }
    }
    if (progress_value) {
        progress_.notify();
    }
    return progress_value;
}

bool websocket_http3_transport::receive() {
    using status_type = http3_client_receive_driver::status_type;
    if (eof_ || received_.size() == receive_capacity) {
        return false;
    }
    auto& quic = driver_.session()->transport();
    const auto id = *stream_id_;
    const auto health = quic.read_health(id);
    if (health.status_ == ruvia::quic_stream_read_status::reset) {
        throw websocket_client_error(websocket_client_error::code_type::protocol_error,
            "upstream reset HTTP/3 WebSocket stream");
    }
    if (health.status_ != ruvia::quic_stream_read_status::would_block &&
        health.status_ != ruvia::quic_stream_read_status::fin &&
        health.status_ != ruvia::quic_stream_read_status::data) {
        throw websocket_client_error(websocket_client_error::code_type::io_error,
            "HTTP/3 WebSocket stream transport failed");
    }
    const auto part = driver_.receiver().drive(id, [&quic](std::uint64_t stream_id, std::span<char> bytes_value) { return quic.read_stream(stream_id, std::as_writable_bytes(bytes_value)); }, receive_capacity - received_.size());
    switch (part.status_) {
        case status_type::blocked:
            return false;
        case status_type::progress:
            return true;
        case status_type::response_complete:
            eof_ = true;
            return true;
        case status_type::stream_reset:
            throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                "upstream reset HTTP/3 WebSocket stream");
        case status_type::stream_error:
        case status_type::peer_stream_ended:
            throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                "invalid HTTP/3 WebSocket stream input");
        case status_type::connection_error:
            driver_.fail(http3_client_session_driver::outcome_type::protocol_error,
                http3_client_protocol_close_code(part.protocol_), "invalid HTTP/3 WebSocket connection input");
        case status_type::transport_error:
            driver_.fail(http3_client_session_driver::outcome_type::transport_error, std::nullopt,
                "HTTP/3 WebSocket stream transport failed");
    }
    return false;
}

// Writes the unaccepted suffix of one segment; true once it is fully accepted.
bool websocket_http3_transport::write_segment(std::span<const char> bytes_value) {
    const auto written = driver_.session()->transport().write_stream(*stream_id_, std::as_bytes(bytes_value));
    if (written.status_ == ruvia::quic_operation_status::would_block ||
        written.status_ == ruvia::quic_operation_status::need_input) {
        return false;
    }
    if (written.status_ != ruvia::quic_operation_status::accepted || written.accepted_ > bytes_value.size()) {
        throw websocket_client_error(websocket_client_error::code_type::io_error,
            "could not write HTTP/3 WebSocket stream");
    }
    write_offset_ += written.accepted_;
    return written.accepted_ == bytes_value.size();
}

bool websocket_http3_transport::drive_output() {
    bool progress_value = false;
    auto& quic = driver_.session()->transport();
    if (open_requested_ && !stream_id_) {
        const auto opened = quic.open_stream(false);
        if (opened.status_ == ruvia::quic_operation_status::accepted) {
            const auto registered = driver_.engine().register_request(opened.stream_id_, http_known_method::connect,
                {.callback_ = on_event, .context_ = this});
            if (registered.scope_ != http3_connection_error_scope::none) {
                throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                    "could not register HTTP/3 WebSocket stream");
            }
            stream_id_ = opened.stream_id_;
            const auto& config = owner_.config_;
            auto head = driver_.engine().encode_websocket_request_head(*stream_id_, config.negotiation_,
                "https", config.authority_, config.target_);
            if (head.index() != 0) {
                throw websocket_client_error(websocket_client_error::code_type::handshake_rejected,
                    "could not encode HTTP/3 WebSocket Extended CONNECT");
            }
            data_plan_.emplace(std::get<0>(head).body_plan_);
            head_.emplace(std::move(std::get<0>(head).headers_));
            progress_value = true;
        } else if (opened.status_ != ruvia::quic_operation_status::would_block &&
                   opened.status_ != ruvia::quic_operation_status::need_input) {
            throw websocket_client_error(websocket_client_error::code_type::io_error,
                "could not open HTTP/3 WebSocket stream");
        }
    }
    if (!stream_id_) {
        return progress_value;
    }
    const auto offset_before = write_offset_;
    if (head_) {
        if (write_segment(head_->frame().subspan(write_offset_))) {
            head_.reset();
            write_offset_ = 0;
            progress_value = true;
        }
    } else if (chunk_pending_) {
        const auto header_size = chunk_.frame_header_size_;
        bool complete = false;
        if (write_offset_ < header_size) {
            complete = write_segment(std::span<const char>(chunk_.frame_header_.data(), header_size).subspan(write_offset_)) &&
                       chunk_.payload_.empty();
        } else {
            complete = write_segment(chunk_.payload_.subspan(write_offset_ - header_size));
        }
        if (complete) {
            if (data_plan_->commit_payload(chunk_.payload_.size(), false).index() != 0) {
                throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                    "HTTP/3 WebSocket DATA plan rejected the payload");
            }
            chunk_ = {};
            chunk_pending_ = false;
            write_offset_ = 0;
            progress_value = true;
        }
    } else if (finish_requested_ && !finished_) {
        const auto concluded = quic.finish_stream(*stream_id_);
        if (concluded == ruvia::quic_operation_status::accepted) {
            if (data_plan_->commit_payload(0, true).index() != 0) {
                throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                    "HTTP/3 WebSocket DATA plan rejected the stream end");
            }
            finished_ = true;
            progress_value = true;
        } else if (concluded != ruvia::quic_operation_status::would_block &&
                   concluded != ruvia::quic_operation_status::need_input) {
            throw websocket_client_error(websocket_client_error::code_type::io_error,
                "could not conclude HTTP/3 WebSocket stream");
        }
    }
    return progress_value || write_offset_ != offset_before;
}

void websocket_http3_transport::record_failure(http3_client_session_driver::outcome_type outcome) noexcept {
    if (const auto failure = driver_.failure()) {
        try {
            std::rethrow_exception(failure);
        } catch (const websocket_client_error&) {
            failure_ = failure;
            return;
        } catch (...) {
        }
    }
    failure_ = std::make_exception_ptr(websocket_client_error(error_code(outcome), "HTTP/3 WebSocket connection failed"));
}

task<void> websocket_http3_transport::drive() {
    const auto outcome = co_await driver_.run(*this);
    if (!stopped_) {
        record_failure(outcome);
    }
    co_await driver_.close();
    driver_.reset_attempt();
    if (!stopped_) {
        owner_.fail_transport();
    }
    progress_.notify();
}

task<void> websocket_http3_transport::wait_for_output() {
    driver_.wake();
    while (head_ || chunk_pending_) {
        check_failure();
        co_await progress_.wait();
    }
    check_failure();
}

void websocket_http3_transport::abandon_output() noexcept {
    // A partly accepted DATA frame cannot be withdrawn and its payload is
    // borrowed from the leaving writer: the stream ends with this transport.
    chunk_ = {};
    chunk_pending_ = false;
    stop();
}

task<std::size_t> websocket_http3_transport::read(std::span<char> output) {
    for (;;) {
        check_failure();
        const auto available = received_.size() - read_offset_;
        if (available != 0) {
            const auto count = std::min(output.size(), available);
            std::memcpy(output.data(), received_.data() + read_offset_, count);
            read_offset_ += count;
            if (read_offset_ == received_.size()) {
                received_.clear();
                read_offset_ = 0;
            }
            driver_.wake();
            co_return count;
        }
        if (eof_) {
            co_return 0;
        }
        driver_.wake();
        co_await progress_.wait();
    }
}

task<void> websocket_http3_transport::write(std::string_view bytes_value) {
    check_failure();
    if (bytes_value.empty()) {
        co_return;
    }
    if (!data_plan_ || finish_requested_ || chunk_pending_ || head_) {
        throw std::logic_error("HTTP/3 WebSocket write requires an open, idle stream");
    }
    auto planned = data_plan_->plan_chunk(bytes_value, false);
    if (planned.index() != 0) {
        throw websocket_client_error(websocket_client_error::code_type::protocol_error,
            "HTTP/3 WebSocket DATA plan rejected the payload");
    }
    chunk_ = std::get<0>(planned);
    chunk_pending_ = true;
    write_offset_ = 0;
    struct borrow_guard final {
        websocket_http3_transport& transport_;
        ~borrow_guard() {
            if (transport_.chunk_pending_) {
                transport_.abandon_output();
            }
        }
    } guard_value{*this};
    co_await wait_for_output();
}

task<void> websocket_http3_transport::finish() {
    check_failure();
    if (!data_plan_ || chunk_pending_ || head_) {
        throw std::logic_error("HTTP/3 WebSocket finish requires an open, idle stream");
    }
    if (!finish_requested_) {
        if (data_plan_->plan_chunk({}, true).index() != 0) {
            throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                "HTTP/3 WebSocket DATA plan rejected the stream end");
        }
        finish_requested_ = true;
    }
    driver_.wake();
    // RFC 9220 closes both stream halves. Retiring the connection before the
    // peer ended its half would cut off its in-flight close output; the
    // owner's close-handshake timer bounds this wait.
    while (!completed_) {
        check_failure();
        co_await progress_.wait();
    }
}

}  // namespace ruvia::detail
