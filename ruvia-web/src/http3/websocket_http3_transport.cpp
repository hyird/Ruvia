#include "http3/websocket_http3_transport.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <utility>
#include <variant>

#include <asio/post.hpp>

#include "ruvia/core/async.h"
#include "ruvia/http/http3_frames.h"
#include "ruvia/http/websocket_client_negotiation.h"

#include "client/websocket_client_state.h"

namespace ruvia::detail {
namespace {
constexpr std::size_t receive_capacity = 64 * 1024;
constexpr std::size_t wire_block_bytes = 16384;
using stream_read_type = ruvia::quic_stream_read_result;
using stream_write_type = ruvia::quic_stream_write_result;
using stream_error_type = ruvia::quic_operation_status;
}  // namespace

websocket_http3_transport::websocket_http3_transport(websocket_client_state& owner_value,
    const worker_handle& worker_value, std::pmr::memory_resource* resource)
    : owner_(owner_value),
      resource_(resource),
      tls_(owner_value.config_.transport_.view(), resource),
      resolver_(owner_value.loop_.io_context(), resource),
      connection_(http3_peer_role::client, resource,
          {.max_active_streams_ = 1, .max_peer_unidirectional_streams_ = ruvia::quic_limits{}.max_streams_, .qpack_max_table_capacity_ = owner_value.config_.qpack_.max_table_capacity_, .qpack_blocked_streams_ = owner_value.config_.qpack_.max_blocked_streams_}),
      drivers_(worker_value, {.resource_ = resource}),
      progress_(worker_value),
      peer_streams_(resource),
      received_(resource),
      outbound_(resource),
      blocked_input_(resource),
      critical_output_{std::pmr::string(resource), std::pmr::string(resource)} {
    peer_streams_.reserve(ruvia::quic_limits{}.max_streams_);
}

void websocket_http3_transport::check_failure() const {
    if (failure_) {
        std::rethrow_exception(failure_);
    }
    owner_.throw_abort();
}

void websocket_http3_transport::wake() noexcept {
    if (session_) {
        session_->notify_work();
    }
}

task<void> websocket_http3_transport::connect() {
    drivers_.spawn(drive());
    while (!connection_.peer_settings()) {
        check_failure();
        co_await progress_.wait();
    }
    check_failure();
    if (!connection_.peer_settings()->enable_connect_protocol_) {
        throw websocket_client_error(websocket_client_error::code_type::handshake_rejected,
            "upstream did not enable HTTP/3 Extended CONNECT");
    }
    open_requested_ = true;
    wake();
    while (!stream_id_) {
        check_failure();
        co_await progress_.wait();
    }
    check_failure();
    std::pmr::vector<http_header_view> headers(resource_);
    headers.reserve(owner_.config_.headers_.size() + 1);
    for (const auto& header : owner_.config_.headers_) {
        headers.emplace_back(header.name_, header.value_);
    }
    if (!owner_.config_.user_agent_.empty()) {
        headers.emplace_back("user-agent", owner_.config_.user_agent_);
    }
    std::pmr::vector<std::string_view> protocols(resource_);
    for (const auto& protocol : owner_.config_.subprotocols_) {
        protocols.emplace_back(protocol);
    }
    websocket_client_negotiation negotiation({.headers_ = headers,
                                                 .subprotocols_ = protocols,
                                                 .deflate_ = owner_.config_.deflate_},
        resource_);
    std::pmr::vector<http3_field_section_field_view> fields(resource_);
    for (const auto& field : negotiation.request_fields()) {
        fields.push_back({field.name(), field.value()});
    }
    auto authority = client_uri_host(owner_.config_.host_, resource_);
    client_port_text_buffer_type port_buffer{};
    authority.append(":");
    authority.append(format_client_port(owner_.port(), port_buffer));
    http3_field_section_limits limits{};
    if (const auto maximum = connection_.peer_settings()->max_field_section_size_) {
        limits.max_decoded_bytes_ = static_cast<std::size_t>(std::min<std::uint64_t>(limits.max_decoded_bytes_, *maximum));
    }
    const auto head = connection_.encode_client_request_head(*stream_id_, {.method_ = "CONNECT",
                                                                              .scheme_ = "https",
                                                                              .authority_ = authority,
                                                                              .path_ = owner_.config_.target_,
                                                                              .fields_ = fields,
                                                                              .protocol_ = "websocket",
                                                                              .peer_enable_connect_protocol_ = true});
    if ((head.index() != 0)) {
        throw websocket_client_error(websocket_client_error::code_type::handshake_rejected,
            "could not encode HTTP/3 WebSocket Extended CONNECT");
    }
    prepare_frame(1, std::get<0>(head).field_section_);
    wake();
    while (!response_) {
        check_failure();
        if (eof_) {
            throw websocket_client_error(websocket_client_error::code_type::handshake_rejected,
                "upstream closed before HTTP/3 WebSocket response");
        }
        co_await progress_.wait();
    }
    check_failure();
    const auto negotiated = negotiation.validate_response(*response_, !eof_);
    if ((negotiated.index() != 0)) {
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
    resolver_.request_stop();
    drivers_.request_stop();
    if (session_) {
        session_->request_stop();
    }
    progress_.notify();
}

task<void> websocket_http3_transport::join() {
    co_await drivers_.join();
    session_.reset();
    (void)connection_.retire();
    std::pmr::string(resource_).swap(received_);
    std::pmr::string(resource_).swap(outbound_);
    std::pmr::string(resource_).swap(blocked_input_);
    for (auto& bytes : critical_output_) {
        std::pmr::string(resource_).swap(bytes);
    }
}

void websocket_http3_transport::prepare_frame(std::uint64_t type, std::span<const char> payload_value) {
    if (!outbound_.empty()) {
        throw std::logic_error("overlapping HTTP/3 WebSocket output blocks");
    }
    std::array<char, 16> header_value{};
    const auto encoded = encode_http3_frame_header(header_value, type, payload_value.size());
    if ((encoded.index() != 0)) {
        std::terminate();
    }
    outbound_.reserve(std::get<0>(encoded) + payload_value.size());
    outbound_.append(header_value.data(), std::get<0>(encoded));
    outbound_.append(payload_value.data(), payload_value.size());
    write_offset_ = 0;
}

void websocket_http3_transport::on_event(void* context_value, const http3_connection_event& event) {
    auto& self = *static_cast<websocket_http3_transport*>(context_value);
    switch (event.kind_) {
        case http3_connection_event_kind::final_head:
            if (self.response_ || !event.head_) {
                throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                    "unexpected HTTP/3 WebSocket response head");
            }
            self.response_.emplace(self.resource_);
            self.response_->status_ = event.head_->status_;
            for (const auto& field : event.head_->headers_) {
                self.response_->headers_.emplace_back(field.name_, field.value_, self.resource_);
            }
            break;
        case http3_connection_event_kind::tunnel_data:
            if (event.body_.size() > receive_capacity - self.received_.size()) {
                throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                    "HTTP/3 WebSocket receive buffer exceeded");
            }
            self.received_.append(event.body_.data(), event.body_.size());
            break;
        case http3_connection_event_kind::message_end:
            self.eof_ = true;
            break;
        case http3_connection_event_kind::reset:
            throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                "upstream reset HTTP/3 WebSocket stream");
        case http3_connection_event_kind::body:
            if (!event.body_.empty()) {
                throw websocket_client_error(websocket_client_error::code_type::handshake_rejected,
                    "upstream rejected HTTP/3 WebSocket handshake");
            }
            break;
        default:
            break;
    }
    self.progress_.notify();
}

bool websocket_http3_transport::receive(std::uint64_t id, bool request) {
    auto& transport = session_->transport();
    if (request) {
        const auto health = transport.read_health(id);
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
        if (eof_ || received_.size() == receive_capacity) {
            return false;
        }
    }
    std::array<char, wire_block_bytes> bytes_value{};
    std::span<const char> input;
    bool fin = false;
    if (request && qpack_blocked_) {
        if (blocked_input_.size() > receive_capacity - received_.size()) {
            return false;
        }
        input = blocked_input_;
        fin = blocked_fin_;
    } else {
        const auto capacity = request ? std::min(bytes_value.size(), receive_capacity - received_.size()) : bytes_value.size();
        const auto read = transport.read_stream(id,
            std::as_writable_bytes(std::span<char>(bytes_value.data(), capacity)));
        if (read.status_ == ruvia::quic_stream_read_status::would_block) {
            return false;
        }
        if (read.status_ != ruvia::quic_stream_read_status::data && read.status_ != ruvia::quic_stream_read_status::fin) {
            throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                "HTTP/3 peer stream ended unexpectedly");
        }
        fin = read.status_ == ruvia::quic_stream_read_status::fin;
        input = std::span<const char>(bytes_value.data(), read.size_);
    }
    const auto result_value = connection_.feed(id, input, fin, false, on_event, this);
    if (result_value.status_ == http3_connection_status::connection_error || result_value.status_ == http3_connection_status::stream_error ||
        result_value.status_ == http3_connection_status::reset || result_value.status_ == http3_connection_status::push_promise_pending) {
        throw websocket_client_error(websocket_client_error::code_type::protocol_error,
            "invalid HTTP/3 WebSocket connection input");
    }
    if (request) {
        if (result_value.status_ == http3_connection_status::qpack_blocked) {
            if (result_value.consumed_bytes_ > input.size()) {
                std::terminate();
            }
            if (qpack_blocked_) {
                blocked_input_.erase(0, result_value.consumed_bytes_);
            } else {
                blocked_input_.assign(input.data() + result_value.consumed_bytes_, input.size() - result_value.consumed_bytes_);
            }
            qpack_blocked_ = true;
            blocked_fin_ = fin;
            return result_value.consumed_bytes_ != 0;
        }
        qpack_blocked_ = false;
        blocked_fin_ = false;
        blocked_input_.clear();
    } else if (fin) {
        (void)transport.close_stream(id);
        std::erase(peer_streams_, id);
    }
    return true;
}

bool websocket_http3_transport::drive_output() {
    bool progress_value = false;
    auto& transport = session_->transport();
    if (open_requested_ && !stream_id_) {
        const auto opened = transport.open_stream(false);
        if (opened.status_ == ruvia::quic_operation_status::accepted) {
            const auto registered = connection_.register_client_request(opened.stream_id_, http_known_method::connect);
            if (registered.status_ != http3_connection_status::need_more_data) {
                throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                    "could not register HTTP/3 WebSocket stream");
            }
            stream_id_ = opened.stream_id_;
            progress_.notify();
            progress_value = true;
        } else if (opened.status_ != ruvia::quic_operation_status::would_block && opened.status_ != ruvia::quic_operation_status::need_input) {
            throw websocket_client_error(websocket_client_error::code_type::io_error,
                "could not open HTTP/3 WebSocket stream");
        }
    }
    if (stream_id_ && !outbound_.empty()) {
        const auto bytes_value = std::span<const char>(outbound_.data() + write_offset_, outbound_.size() - write_offset_);
        const auto written = transport.write_stream(*stream_id_, std::as_bytes(bytes_value));
        if (written.status_ == ruvia::quic_operation_status::accepted) {
            if (written.accepted_ == 0 || written.accepted_ > bytes_value.size()) {
                std::terminate();
            }
            write_offset_ += written.accepted_;
            progress_value = true;
            if (write_offset_ == outbound_.size()) {
                outbound_.clear();
                write_offset_ = 0;
                progress_.notify();
            }
        } else if (written.status_ != ruvia::quic_operation_status::would_block) {
            throw websocket_client_error(websocket_client_error::code_type::io_error,
                "could not write HTTP/3 WebSocket stream");
        }
    }
    if (stream_id_ && finish_requested_ && outbound_.empty() && !fin_prepared_) {
        const auto result_value = transport.finish_stream(*stream_id_);
        if (result_value == ruvia::quic_operation_status::accepted) {
            fin_prepared_ = true;
            finished_ = true;
            progress_.notify();
            progress_value = true;
        } else if (result_value != ruvia::quic_operation_status::would_block && result_value != ruvia::quic_operation_status::need_input) {
            throw websocket_client_error(websocket_client_error::code_type::io_error,
                "could not conclude HTTP/3 WebSocket stream");
        }
    }
    for (std::size_t index = 0; index != critical_output_.size(); ++index) {
        auto& bytes_value = critical_output_[index];
        auto& offset = critical_offset_[index];
        const auto pending = index == 0 ? connection_.pending_qpack_encoder_output() : connection_.pending_qpack_decoder_output();
        if (bytes_value.empty() && !pending.empty()) {
            bytes_value.assign(pending.data(), std::min(pending.size(), wire_block_bytes));
            offset = 0;
            owner_.arm(critical_timers_[index], owner_.config_.write_timeout_, websocket_client_state::abort_reason_type::timeout);
        }
        if (bytes_value.empty()) {
            continue;
        }
        const auto written = session_->write_critical_stream(index == 0 ? ruvia::http3_critical_stream_output::stream_kind::qpack_encoder : ruvia::http3_critical_stream_output::stream_kind::qpack_decoder,
            std::span<const char>(bytes_value.data() + offset, bytes_value.size() - offset));
        if (written.status_ == ruvia::quic_operation_status::accepted) {
            if (written.accepted_ == 0 || written.accepted_ > bytes_value.size() - offset) {
                std::terminate();
            }
            const bool consumed = index == 0 ? connection_.consume_qpack_encoder_output(written.accepted_) : connection_.consume_qpack_decoder_output(written.accepted_);
            if (!consumed) {
                std::terminate();
            }
            offset += written.accepted_;
            progress_value = true;
            owner_.arm(critical_timers_[index], owner_.config_.write_timeout_, websocket_client_state::abort_reason_type::timeout);
            if (offset == bytes_value.size()) {
                bytes_value.clear();
                offset = 0;
                owner_.disarm(critical_timers_[index]);
            }
        } else if (written.status_ != ruvia::quic_operation_status::would_block) {
            throw websocket_client_error(websocket_client_error::code_type::io_error,
                "HTTP/3 QPACK critical stream failed");
        }
    }
    return progress_value;
}

task<void> websocket_http3_transport::drive() {
    try {
        const auto deadline_value = std::chrono::steady_clock::now() + owner_.config_.connect_timeout_;
        const auto resolved = co_await resolver_.resolve(owner_.config_.host_, owner_.port(), deadline_value);
        if (stopped_) {
            co_return;
        }
        if (resolved.status_ != http3_quic_client_endpoint_resolver::status_type::resolved || resolved.endpoints_.empty()) {
            throw websocket_client_error(websocket_client_error::code_type::resolve_failed,
                "could not resolve HTTP/3 WebSocket peer");
        }
        // Each address receives part of the same connect deadline; successful
        // HTTP/3 admission retains that session for the connection's lifetime.
        bool established = false;
        for (std::size_t index = 0; index < resolved.endpoints_.size() && !stopped_; ++index) {
            session_.emplace(owner_.loop_.io_context(), resolved.endpoints_[index], owner_.config_.host_,
                tls_, connection_.local_settings());
            const auto now = std::chrono::steady_clock::now();
            const auto attempt_deadline = now + (deadline_value - now) / (resolved.endpoints_.size() - index);
            while (!stopped_) {
                const auto pump = session_->pump();
                if (pump.status_ == http3_quic_client_socket_session::pump_status_type::fatal || pump.status_ == http3_quic_client_socket_session::pump_status_type::closed) {
                    break;
                }
                if (pump.critical_streams_ready_) {
                    established = true;
                    break;
                }
                if (std::chrono::steady_clock::now() >= attempt_deadline) {
                    break;
                }
                const auto wake_reason = co_await session_->wait_for_activity(pump, attempt_deadline);
                if (wake_reason == http3_quic_client_socket_session::wake_reason_type::fatal) {
                    break;
                }
            }
            if (established || stopped_) {
                break;
            }
            session_->close();
            session_.reset();
        }
        if (!stopped_ && !established) {
            throw websocket_client_error(websocket_client_error::code_type::connect_failed,
                "could not establish HTTP/3 WebSocket transport");
        }
        unsigned active_ticks = 0;
        while (!stopped_) {
            (void)session_->consume_work_notification();
            const auto pump = session_->pump();
            if (pump.status_ == http3_quic_client_socket_session::pump_status_type::fatal || pump.status_ == http3_quic_client_socket_session::pump_status_type::closed) {
                throw websocket_client_error(websocket_client_error::code_type::io_error,
                    "HTTP/3 WebSocket connection failed");
            }

            auto& transport = session_->transport();
            const auto accepted = transport.accept_streams();
            if (accepted.status_ != ruvia::quic_operation_status::accepted &&
                accepted.status_ != ruvia::quic_operation_status::need_input &&
                accepted.status_ != ruvia::quic_operation_status::would_block) {
                throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                    "could not accept HTTP/3 peer streams");
            }
            for (std::size_t index = 0; index < accepted.size_; ++index) {
                if (!accepted.streams_[index].readable_ || accepted.streams_[index].writable_ ||
                    peer_streams_.size() == ruvia::quic_limits{}.max_streams_) {
                    throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                        "invalid HTTP/3 peer stream");
                }
                peer_streams_.push_back(accepted.streams_[index].stream_id_);
            }
            bool progress_value = accepted.size_ != 0 || pump.received_ != 0 || pump.sent_ != 0 || pump.critical_output_progress_;
            for (std::size_t index = 0; index < peer_streams_.size();) {
                const auto id = peer_streams_[index];
                progress_value |= receive(id, false);
                if (index < peer_streams_.size() && peer_streams_[index] == id) {
                    ++index;
                }
            }
            if (stream_id_) {
                progress_value |= receive(*stream_id_, true);
            }
            if (pump.critical_streams_ready_) {
                progress_value |= drive_output();
            }
            if (progress_value) {
                progress_.notify();
            }
            if (progress_value && ++active_ticks < 32) {
                continue;
            }
            if (progress_value) {
                (void)co_await ruvia::async_asio([this](auto handler) {
                    asio::post(owner_.loop_.executor(), [handler = std::move(handler)]() mutable { handler(std::error_code{}); });
                });
            } else {
                const auto reason = co_await session_->wait_for_activity(pump);
                if (reason == http3_quic_client_socket_session::wake_reason_type::fatal) {
                    throw websocket_client_error(websocket_client_error::code_type::io_error,
                        "HTTP/3 WebSocket socket wait failed");
                }
            }
            active_ticks = 0;
        }
    } catch (...) {
        if (!stopped_ && !failure_) {
            failure_ = std::current_exception();
        }
        owner_.close_on_worker(websocket_client_state::abort_reason_type::closing);
    }
    for (auto& timer : critical_timers_) {
        owner_.disarm(timer);
    }
    if (session_) {
        session_->close();
    }
    progress_.notify();
}

task<void> websocket_http3_transport::wait_for_output() {
    wake();
    while (!outbound_.empty()) {
        check_failure();
        co_await progress_.wait();
    }
    check_failure();
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
            wake();
            co_return count;
        }
        if (eof_) {
            co_return 0;
        }
        wake();
        co_await progress_.wait();
    }
}

task<void> websocket_http3_transport::write(std::string_view bytes_value) {
    while (!bytes_value.empty()) {
        check_failure();
        const auto count = std::min(bytes_value.size(), wire_block_bytes);
        prepare_frame(0, std::span<const char>(bytes_value.data(), count));
        co_await wait_for_output();
        bytes_value.remove_prefix(count);
    }
}

task<void> websocket_http3_transport::finish() {
    check_failure();
    finish_requested_ = true;
    wake();
    while (!finished_) {
        check_failure();
        co_await progress_.wait();
    }
}

}  // namespace ruvia::detail
