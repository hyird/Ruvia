#include <array>
#include <chrono>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <variant>

#include "ruvia/http/http3_websocket_handshake.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_connect_udp.h"

#include "context/context_access.h"
#include "http/http_tunnel_session.h"
#include "http3/http3_buffered_request_dispatch.h"
#include "http3/http3_connection_driver.h"
#include "http3/http3_server_connection.h"
#include "http3/http3_server_request_entry.h"
#include "router/route_endpoint.h"
#include "router/route_resolution.h"
#include "router/route_table.h"
#include "server/http_upgrade_route.h"
#include "websocket/http_websocket_connection.h"
#include "websocket/http_websocket_session.h"
#include "websocket/websocket_response_headers.h"

// CONNECT, CONNECT-UDP and WebSocket tunnels over HTTP/3 request streams, and
// HTTP Datagrams bound to them.

namespace ruvia::detail {
namespace {

[[nodiscard]] std::int64_t steady_now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] bool has_unsupported_websocket_version(const http_request& request) noexcept {
    std::size_t version_count = 0;
    std::string_view version;
    for (const auto& header : request.headers()) {
        if (!http_ascii_equals_ignore_case(header.name(), "sec-websocket-version")) {
            continue;
        }
        if (++version_count != 1) {
            return false;
        }
        version = header.value();
    }
    return version_count == 1 && version != "13";
}

class http3_tunnel_transport final {
public:
    explicit http3_tunnel_transport(http3_buffered_request_dispatch& dispatch) noexcept
        : dispatch_(dispatch) {}
    [[nodiscard]] task<std::optional<http_datagram_input>> read_datagram_input() {
        return dispatch_.read_datagram_input();
    }
    [[nodiscard]] http_datagram_session_config datagram_config() const {
        return dispatch_.datagram_config();
    }
    void send_datagram(std::span<const std::byte> bytes_value) {
        dispatch_.send_datagram(bytes_value);
    }
    [[nodiscard]] task<http_stream_read_result> read_more(std::pmr::string& buffer) {
        return dispatch_.read_tunnel(buffer);
    }
    [[nodiscard]] task<std::error_code> write_bytes(std::string_view bytes_value, http_stream_end end) {
        return dispatch_.write_tunnel(bytes_value, end);
    }
    void abort() noexcept {
        dispatch_.abort_tunnel();
    }

private:
    http3_buffered_request_dispatch& dispatch_;
};

class http3_websocket_transport final {
public:
    explicit http3_websocket_transport(http3_buffered_request_dispatch& dispatch) noexcept
        : dispatch_(dispatch) {}

    [[nodiscard]] asio::any_io_executor executor() const noexcept {
        return dispatch_.executor();
    }

    [[nodiscard]] task<http_stream_read_result> read_more(std::pmr::string& buffer) {
        return dispatch_.read_tunnel(buffer);
    }

    [[nodiscard]] task<std::error_code> write_bytes(std::string_view bytes_value,
        websocket_transport_disposition disposition) {
        return dispatch_.write_tunnel(bytes_value, disposition == websocket_transport_disposition::end_transport ? http_stream_end::end : http_stream_end::keep_open);
    }

    void abort() noexcept {
        dispatch_.abort_tunnel();
    }

private:
    http3_buffered_request_dispatch& dispatch_;
};

}  // namespace

task<http3_buffered_request_dispatch::upgrade_result_type>
http3_buffered_request_dispatch::run_tunnel_handler(const context_services& services) {
    const auto& request = lease_->request().request();
    const auto& resolved = *admission_->resolved();
    const auto& endpoint = *resolved.route().endpoint().tunnel();
    const bool udp = admission_->udp_tunnel();
    using session_type = http_tunnel_session<http3_tunnel_transport>;
    std::optional<session_type> tunnel_session;
    auto establish = [&](context& context_value) -> task<void> {
        auto response = context_access::streaming_head(context_value);
        if (udp) {
            auto negotiated = prepare_http_connect_udp_response(std::move(response), http_protocol_version::http3);
            if (negotiated.index() != 0) {
                throw std::invalid_argument("invalid CONNECT-UDP response metadata");
            }
            response = std::move(std::get<0>(negotiated));
        }
        auto head = session_.encode_connect_response_head(stream_id_, response);
        if (head.index() != 0) {
            if (std::get<1>(head).kind_ == http3_response_head_error::peer_field_section_limit) {
                reject_peer_field_section();
            }
            throw std::invalid_argument("HTTP/3 CONNECT response head rejected");
        }
        const auto& encoded = std::get<0>(head);
        if (!response_field_section_allowed(encoded.headers_.decoded_field_section_size())) {
            reject_peer_field_section();
        }
        if (!owner_.attach_tunnel_scanner(stream_id_, scanner_entry_)) {
            throw std::runtime_error("HTTP/3 tunnel scanner attachment failed");
        }
        context_access::commit_upgrade(context_value);
        if (const auto error = co_await publish_tunnel_handshake(encoded.headers_.frame(),
                http3_data_write_plan(encoded.body_plan_, encoded.declared_content_length_));
            error) {
            throw std::system_error(error, "HTTP/3 CONNECT response publication");
        }
        tunnel_session.emplace(http3_tunnel_transport(*this), services_.worker(), *context_value.pool());
        co_await invoke_tunnel_handler(*tunnel_session, scanner_entry_, resolved.stream_handler(), context_value);
    };
    auto finish = [&](session_type& session_value, std::exception_ptr failure) {
        return finish_tunnel_session(session_value, std::move(failure), options_.connection_failure_,
            services_.get_conn_info().remote().address(), scanner_entry_, endpoint.config().peer_transport_fin_timeout_);
    };
    auto outcome = co_await run_http_upgrade_route(http_upgrade_kind::tunnel, routes_, request, resolved,
        *request_memory_, services, tunnel_session, establish, finish);
    if (outcome.upgraded()) {
        if (!cancellation_requested() && !tunnel_aborted_) {
            (void)co_await wait_tunnel_receive_end();
        }
        co_return cancellation_requested() || tunnel_aborted_ ? run_status_type::cancelled
                                                               : run_status_type::tunnel_complete;
    }
    if (auto* buffered = outcome.response()) {
        co_return std::move(*buffered);
    }
    // The head may be partially published: abandon the stream.
    abort_tunnel();
    co_return run_status_type::cancelled;
}

task<http3_buffered_request_dispatch::upgrade_result_type>
http3_buffered_request_dispatch::run_websocket_handler(const context_services& services) {
    const auto& request = lease_->request().request();
    const auto& resolved = *admission_->resolved();
    const auto& endpoint = *resolved.route().endpoint().get_websocket();
    peer_transport_fin_timeout_ = endpoint.lifecycle().peer_transport_fin_timeout_;
    const auto protocol = lease_->request().extended_connect_protocol();
    const auto validation = validate_http3_websocket_handshake(
        request, protocol, !session_.tunnel_receive_ended(stream_id_));

    if (validation.index() != 0) {
        const auto& failure = std::get<1>(validation);
        const auto unsupported = failure.kind() ==
                                     http3_websocket_handshake_failure::kind_type::unsupported_version ||
                                 (request.known_method() == http_known_method::connect &&
                                     http_ascii_equals_ignore_case(protocol, "websocket") &&
                                     has_unsupported_websocket_version(request));
        const auto protocol_error = failure.protocol_error();
        auto response = co_await routes_.handle_error(request, *request_memory_,
            http_error_info({.status_ = protocol_error.status(),
                .code_ = unsupported ? "websocket_version_unsupported"
                                     : "invalid_websocket_handshake",
                .message_ = protocol_error.what()}),
            admission_->services());
        if (unsupported) {
            response.remove_header("Sec-WebSocket-Version");
            response.header("Sec-WebSocket-Version", "13");
        } else {
            failure.apply_required_response_headers(response);
        }
        co_return std::move(response);
    }

    using session_type = websocket_connection<http3_websocket_transport>;
    std::optional<session_type> websocket_session;
    auto establish = [&](context& context_value) -> task<void> {
        const auto response_headers_value = websocket_response_headers(context_value);
        auto handshake = make_http3_websocket_handshake(request, protocol, true,
            {.supported_subprotocols_ = endpoint.subprotocols(),
                .response_headers_ = response_headers_value,
                .resource_ = request_memory_->resource(),
                .deflate_ = endpoint.deflate()});
        if (handshake.index() != 0) {
            throw std::runtime_error("HTTP/3 WebSocket handshake construction failed");
        }
        if (!owner_.attach_tunnel_scanner(stream_id_, scanner_entry_)) {
            throw std::runtime_error("HTTP/3 WebSocket scanner attachment failed");
        }
        context_access::commit_upgrade(context_value);
        // The accepted Extended CONNECT is a 200 tunnel whose DATA frames carry
        // WebSocket frames.
        if (const auto error = co_await publish_tunnel_handshake(std::get<0>(handshake).headers_frame(),
                http3_data_write_plan(plan_http_response_body(http_known_method::connect, http_status::ok), std::nullopt));
            error) {
            throw std::system_error(error, "failed to publish HTTP/3 WebSocket handshake");
        }
        websocket_session.emplace(http3_websocket_transport{*this}, services_.worker(),
            scanner_entry_, endpoint.lifecycle(),
            protocol_byte_limit::limited(options_.max_websocket_message_bytes_),
            session_.inbound_buffer_pool(), std::string_view{}, std::get<0>(handshake).compression(),
            endpoint.deflate().compression_level_);
        co_await invoke_websocket_handler(*websocket_session, scanner_entry_, resolved.stream_handler(), context_value);
    };
    auto finish = [&](session_type& session_value, std::exception_ptr failure) {
        return finish_websocket_session(session_value, std::move(failure), options_.connection_failure_,
            services_.get_conn_info().remote().address());
    };
    auto outcome = co_await run_http_upgrade_route(http_upgrade_kind::websocket, routes_, request, resolved,
        *request_memory_, services, websocket_session, establish, finish);
    if (cancellation_requested()) {
        co_return run_status_type::cancelled;
    }
    if (outcome.upgraded()) {
        co_return co_await wait_tunnel_receive_end() ? run_status_type::tunnel_complete : run_status_type::cancelled;
    }
    if (auto* buffered = outcome.response()) {
        co_return std::move(*buffered);
    }
    abort_tunnel();
    co_return run_status_type::cancelled;
}

task<std::error_code> http3_buffered_request_dispatch::publish_tunnel_handshake(
    std::span<const char> headers_frame, http3_data_write_plan data_plan) {
    if (!on_worker() || headers_frame.empty() || interim_output_.final_committed() || output_pending()) {
        co_return std::make_error_code(std::errc::invalid_argument);
    }
    commit_final_response();
    tunnel_data_plan_.emplace(data_plan);
    // Tunnel establishment is part of publishing this head: it happens as soon
    // as QUIC accepted the last 2xx byte.
    tunnel_establish_pending_ = true;
    pending_head_ = headers_frame;
    if (!co_await await_output()) {
        co_return std::make_error_code(std::errc::operation_canceled);
    }
    while (tunnel_establish_pending_) {
        if (cancellation_requested() || tunnel_aborted_) {
            co_return std::make_error_code(std::errc::operation_canceled);
        }
        co_await tunnel_output_available_.wait();
    }
    if (cancellation_requested() || tunnel_aborted_) {
        co_return std::make_error_code(std::errc::operation_canceled);
    }
    co_return std::error_code{};
}

void http3_buffered_request_dispatch::notify_tunnel_input() noexcept {
    if (!on_worker()) {
        std::terminate();
    }
    tunnel_input_available_.notify();
}

task<std::optional<http_datagram_input>> http3_buffered_request_dispatch::read_datagram_input() {
    if (!on_worker()) {
        throw std::logic_error("HTTP Datagram read requires its worker");
    }
    for (;;) {
        if (cancellation_requested() || tunnel_aborted_) {
            throw std::system_error(std::make_error_code(std::errc::operation_canceled));
        }
        if (auto native = session_.take_datagram(stream_id_)) {
            co_return http_datagram_input{std::move(*native), true};
        }
        std::array<char, 4096> bytes_value{};
        const auto result_value = session_.read_tunnel_data(stream_id_, bytes_value);
        if (result_value.reset_ || result_value.ended_) {
            disarm_peer_transport_fin_timeout();
        }
        if (result_value.overflow_) {
            abort_tunnel();
            throw std::system_error(std::make_error_code(std::errc::no_buffer_space));
        }
        if (result_value.reset_) {
            throw std::system_error(std::make_error_code(std::errc::connection_reset));
        }
        if (result_value.bytes_ != 0) {
            owner_.notify_activation();
            co_return http_datagram_input{std::pmr::string(bytes_value.data(), result_value.bytes_, worker_.resource()), false};
        }
        if (result_value.ended_) {
            co_return std::nullopt;
        }
        co_await tunnel_input_available_.wait();
    }
}

void http3_buffered_request_dispatch::send_datagram(std::span<const std::byte> bytes_value) {
    if (!on_worker() || !tunnel_established_ || stream_output_ended_ || cancellation_requested() || tunnel_aborted_) {
        throw std::runtime_error("HTTP Datagram sending direction is closed");
    }
    owner_.send_datagram(bytes_value);
}

task<http_stream_read_result> http3_buffered_request_dispatch::read_tunnel(std::pmr::string& buffer) {
    if (!on_worker()) {
        co_return http_stream_read_result::make_failure(
            std::make_error_code(std::errc::operation_not_permitted));
    }
    for (;;) {
        if (cancellation_requested() || tunnel_aborted_) {
            co_return http_stream_read_result::make_failure(
                std::make_error_code(std::errc::operation_canceled));
        }
        std::array<char, 4096> bytes_value{};
        const auto result_value = session_.read_tunnel_data(stream_id_, bytes_value);
        if (result_value.reset_ || result_value.ended_) {
            disarm_peer_transport_fin_timeout();
        }
        if (result_value.overflow_) {
            abort_tunnel();
            co_return http_stream_read_result::make_failure(
                std::make_error_code(std::errc::no_buffer_space));
        }
        if (result_value.reset_) {
            co_return http_stream_read_result::make_failure(
                std::make_error_code(std::errc::connection_reset));
        }
        if (result_value.bytes_ != 0) {
            buffer.append(bytes_value.data(), result_value.bytes_);
            owner_.notify_activation();
            co_return http_stream_read_result::make_data();
        }
        if (result_value.ended_) {
            co_return http_stream_read_result::make_end();
        }
        co_await tunnel_input_available_.wait();
    }
}

task<std::error_code> http3_buffered_request_dispatch::write_tunnel(
    std::string_view bytes_value, http_stream_end disposition) {
    if (!on_worker() || !tunnel_established_ || !tunnel_data_plan_ || stream_output_ended_ ||
        output_pending() || fin_pending_) {
        co_return std::make_error_code(std::errc::operation_not_permitted);
    }
    if (!bytes_value.empty()) {
        const auto planned = tunnel_data_plan_->plan_chunk(std::span<const char>(bytes_value.data(), bytes_value.size()), false);
        if (planned.index() != 0) {
            abort_tunnel();
            co_return std::make_error_code(std::errc::message_size);
        }
        const auto& chunk = std::get<0>(planned);
        pending_head_ = std::span<const char>(chunk.frame_header_.data(), chunk.frame_header_size_);
        pending_body_ = chunk.payload_;
        if (!co_await await_output()) {
            co_return std::make_error_code(std::errc::operation_canceled);
        }
        if (tunnel_data_plan_->commit_payload(bytes_value.size(), false).index() != 0) {
            abort_tunnel();
            co_return std::make_error_code(std::errc::message_size);
        }
    }
    if (disposition == http_stream_end::end) {
        if (tunnel_data_plan_->plan_chunk({}, true).index() != 0) {
            abort_tunnel();
            co_return std::make_error_code(std::errc::message_size);
        }
        fin_pending_ = true;
        notify_output();
        while (!stream_output_ended_) {
            if (cancellation_requested() || tunnel_aborted_) {
                co_return std::make_error_code(std::errc::operation_canceled);
            }
            co_await tunnel_output_available_.wait();
        }
        if (tunnel_data_plan_->commit_payload(0, true).index() != 0) {
            std::terminate();
        }
    }
    co_return std::error_code{};
}

task<bool> http3_buffered_request_dispatch::wait_tunnel_receive_end() {
    if (!on_worker()) {
        co_return false;
    }
    for (;;) {
        if (cancellation_requested() || tunnel_aborted_) {
            co_return false;
        }
        const auto result_value = session_.read_tunnel_data(stream_id_, {});
        if (result_value.reset_ || result_value.overflow_) {
            disarm_peer_transport_fin_timeout();
            co_return false;
        }
        if (result_value.ended_) {
            disarm_peer_transport_fin_timeout();
            co_return true;
        }
        co_await tunnel_input_available_.wait();
    }
}

void http3_buffered_request_dispatch::peer_transport_fin_timeout_tick(void* target, std::int64_t now_ms) noexcept {
    auto& dispatch = *static_cast<http3_buffered_request_dispatch*>(target);
    if (!dispatch.on_worker()) {
        std::terminate();
    }
    const auto peer_state = dispatch.session_.read_tunnel_data(dispatch.stream_id_, {});
    if (peer_state.ended_ || peer_state.reset_ || peer_state.overflow_) {
        dispatch.disarm_peer_transport_fin_timeout();
        return;
    }
    if (now_ms >= dispatch.peer_transport_fin_deadline_ms_) {
        dispatch.abort_tunnel();
    }
}

void http3_buffered_request_dispatch::arm_peer_transport_fin_timeout() noexcept {
    if (!on_worker() || peer_fin_timeout_armed_ ||
        peer_transport_fin_timeout_ <= std::chrono::milliseconds::zero()) {
        return;
    }
    const auto peer_state = session_.read_tunnel_data(stream_id_, {});
    if (peer_state.ended_ || peer_state.reset_ || peer_state.overflow_) {
        return;
    }
    const auto now = steady_now_ms();
    const auto timeout = peer_transport_fin_timeout_.count();
    peer_transport_fin_deadline_ms_ = timeout > (std::numeric_limits<std::int64_t>::max)() - now
                                          ? (std::numeric_limits<std::int64_t>::max)()
                                          : now + timeout;
    scanner_entry_.register_periodic_check(
        peer_transport_fin_check_, this, &http3_buffered_request_dispatch::peer_transport_fin_timeout_tick);
    peer_fin_timeout_armed_ = true;
}

void http3_buffered_request_dispatch::disarm_peer_transport_fin_timeout() noexcept {
    peer_transport_fin_check_.reset();
    peer_fin_timeout_armed_ = false;
    peer_transport_fin_deadline_ms_ = 0;
}

void http3_buffered_request_dispatch::abort_tunnel() noexcept {
    if (!on_worker()) {
        std::terminate();
    }
    if (tunnel_aborted_) {
        return;
    }
    disarm_peer_transport_fin_timeout();
    tunnel_aborted_ = true;
    tunnel_input_available_.notify();
    tunnel_output_available_.notify();
    owner_.abort_tunnel(stream_id_);
}

asio::any_io_executor http3_buffered_request_dispatch::executor() const noexcept {
    return owner_.executor_;
}

bool http3_server_connection::attach_tunnel_scanner(
    std::uint64_t stream_id, connection_scanner::entry_type& entry_value) noexcept {
    if (!on_worker() || connection_scanner_ == nullptr) {
        return false;
    }
    auto* slot = requests_.find(stream_id);
    if (slot == nullptr || slot->entry_ == nullptr || &slot->entry_->scanner_entry_ != &entry_value) {
        return false;
    }
    if (slot->entry_->scanner_registered_) {
        return true;
    }
    connection_scanner_->register_entry(entry_value);
    slot->entry_->scanner_registered_ = true;
    return true;
}

void http3_server_connection::tunnel_output_ready(std::uint64_t stream_id) noexcept {
    if (!on_worker()) {
        std::terminate();
    }
    auto* slot = requests_.find(stream_id);
    if (slot == nullptr || slot->entry_ == nullptr || slot->entry_->output_terminal_ ||
        slot->entry_->cancel_requested_ || !slot->entry_->output_enabled_) {
        return;
    }
    enqueue_for_demand(*slot, true);
}

void http3_server_connection::abort_tunnel(std::uint64_t stream_id) noexcept {
    if (!on_worker()) {
        std::terminate();
    }
    auto* slot = requests_.find(stream_id);
    if (slot == nullptr || slot->entry_ == nullptr || slot->entry_->retirement_granted_ ||
        slot->entry_->cancel_requested_) {
        return;
    }
    auto& entry_value = *slot->entry_;
    // Match deadline cancellation: fence publication, tombstone input, then
    // reset the stream even if output FIN was already accepted.
    entry_value.output_enabled_ = false;
    entry_value.cancel_requested_ = true;
    output_.remove(entry_value.slot_);
    retire_request_input(stream_id);
    reset_stream(*slot);
    cancel_entry(entry_value, request_status_type::cancelled);
}

void http3_server_connection::receive_datagram(const http3_datagram_view& datagram) noexcept {
    if (stop_requested_) {
        return;
    }
    const auto code = static_cast<http3_connection_error_code>(http3_datagram_error_code);
    try {
        const auto status = session_.receive_datagram(datagram);
        auto* slot = requests_.find(datagram.stream_id_);
        if (status == http3_datagram_receive_status::connection_error) {
            close_connection(code);
        } else if (status == http3_datagram_receive_status::stream_error && slot != nullptr) {
            reset_stream(*slot, code);
            retire_request_input(slot->stream_id_);
            if (slot->entry_ != nullptr) {
                cancel_entry(*slot->entry_, request_status_type::cancelled);
            }
        } else if (status == http3_datagram_receive_status::deliver && slot != nullptr && slot->entry_ != nullptr) {
            slot->entry_->dispatch_.notify_tunnel_input();
        }
    } catch (...) {
        close_connection(http3_connection_error_code::excessive_load);
    }
}

void http3_server_connection::send_datagram(std::span<const std::byte> datagram) {
    if (stop_requested_ || transport_retired_) {
        throw std::runtime_error("HTTP Datagram output is unavailable");
    }
    transport_.send_datagram(datagram);
}

}  // namespace ruvia::detail
