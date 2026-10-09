#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <asio/connect.hpp>
#include <openssl/rand.h>

#include "ruvia/core/async.h"
#include "ruvia/core/tcp_socket_options.h"
#include "ruvia/http/http1_client_request_writer.h"
#include "ruvia/http/http1_client_response_parser.h"
#include "ruvia/http/http1_close_policy.h"
#include "ruvia/http/http1_websocket_client_handshake.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_limits.h"

#include "client/client_transport.h"
#include "client/websocket_client_internal.h"
#include "client/websocket_client_state.h"

namespace ruvia::detail {

task<void> websocket_client_state::connect() {
    return connect_owned(shared_from_this());
}

task<void> websocket_client_state::establish_transport() {
    arm(connect_timer_, config_.connect_timeout_, abort_reason_type::timeout);

    client_port_text_buffer_type port_buffer{};
    const auto port_text = format_client_port(port(), port_buffer);
    auto resolved =
        co_await ruvia::async_asio<asio::ip::tcp::resolver::results_type>([&](auto handler) mutable {
            resolver_.async_resolve(config_.host_, port_text, std::move(handler));
        });
    throw_abort();
    if (resolved.error_code()) {
        throw websocket_client_error(
            websocket_client_error::code_type::resolve_failed, resolved.error_code().message());
    }

    auto endpoints = std::move(resolved).take_result();
    auto connected = co_await ruvia::async_asio([&](auto handler) mutable {
        asio::async_connect(stream_.lowest_layer(), endpoints, std::move(handler));
    });
    throw_abort();
    if (connected.error_code()) {
        throw websocket_client_error(
            websocket_client_error::code_type::connect_failed, connected.error_code().message());
    }

    const auto transport = config_.transport_.view();
    ruvia::apply_tcp_socket_policies(
        stream_.next_layer(), transport.tcp_no_delay_, transport.tcp_keep_alive_);

    if (config_.scheme_ == websocket_scheme::wss) {
        co_await perform_tls_handshake();
    }
}

task<void> websocket_client_state::perform_tls_handshake() {
    const auto tls_setup = prepare_client_tls_stream(
        stream_, config_.host_, config_.transport_.view(), config_.protocol_ == websocket_client_protocol::http2 ? client_alpn_mode::http2 : client_alpn_mode::http11);
    if (tls_setup != client_tls_setup_error::none) {
        throw websocket_client_error(
            websocket_client_error::code_type::tls_failed, client_tls_setup_error_message(tls_setup));
    }

    auto handshake = co_await ruvia::async_asio([&](auto handler) mutable {
        stream_.async_handshake(asio::ssl::stream_base::client, std::move(handler));
    });
    throw_abort();
    if (handshake.error_code()) {
        throw websocket_client_error(
            websocket_client_error::code_type::tls_failed, handshake.error_code().message());
    }

    const auto alpn = selected_client_alpn(stream_.native_handle());
    if ((config_.protocol_ == websocket_client_protocol::http2 && alpn != "h2") ||
        (config_.protocol_ == websocket_client_protocol::http1 && !alpn.empty() && alpn != "http/1.1")) {
        throw websocket_client_error(websocket_client_error::code_type::handshake_rejected,
            "upstream did not negotiate the requested HTTP protocol for WebSocket");
    }
}

task<void> websocket_client_state::connect_owned(std::shared_ptr<websocket_client_state> state_value) {
    state_value->require_current();
    auto expected = phase_type::fresh;
    if (!state_value->phase_.compare_exchange_strong(
            expected, phase_type::connecting, std::memory_order_acq_rel)) {
        throw websocket_client_error(websocket_client_error::code_type::invalid_state,
            "WebSocket client connect may only be started once");
    }
    state_value->abort_reason_ = abort_reason_type::none;
    state_value->connect_in_flight_ = true;
    try {
        if (state_value->config_.protocol_ == websocket_client_protocol::http3) {
            state_value->arm(state_value->connect_timer_, state_value->config_.connect_timeout_, abort_reason_type::timeout);
            state_value->http3_.emplace(*state_value, state_value->worker_, state_value->memory_.resource());
            co_await state_value->http3_->connect();
        } else {
            co_await state_value->establish_transport();
        }
        if (state_value->config_.protocol_ == websocket_client_protocol::http2) {
            state_value->http2_.emplace(*state_value, state_value->worker_, state_value->memory_.resource());
            co_await state_value->http2_->connect();
        } else if (state_value->config_.protocol_ == websocket_client_protocol::http1) {
            co_await state_value->perform_handshake();
        }
        state_value->protocol_.emplace(websocket_connection_options{
            .resource_ = state_value->memory_.resource(),
            .message_limit_ = protocol_byte_limit::limited(state_value->config_.max_message_bytes_),
            .compression_ = state_value->negotiated_compression_,
            .role_ = websocket_connection_role::client,
            .mask_key_generator_ = &websocket_client_state::generate_mask,
            .compression_level_ = state_value->config_.compression_level_});
        (void)state_value->protocol_->feed(state_value->input_);
        std::pmr::string(state_value->input_.get_allocator()).swap(state_value->input_);
        state_value->disarm(state_value->connect_timer_);
        auto open = phase_type::connecting;
        if (!state_value->phase_.compare_exchange_strong(
                open, phase_type::open, std::memory_order_acq_rel, std::memory_order_acquire)) {
            throw websocket_client_error(
                websocket_client_error::code_type::closing, "WebSocket client closed while connecting");
        }
        state_value->last_active_ms_ = websocket_steady_now_ms();
        state_value->liveness_state_ = websocket_liveness_idle{};
        if (state_value->config_.heartbeat_.ping_interval_.has_value()) {
            state_value->arm_heartbeat_timer(*state_value->config_.heartbeat_.ping_interval_);
        }
        state_value->connect_in_flight_ = false;
        state_value->close_state_.notify_progress();
    } catch (...) {
        state_value->disarm(state_value->connect_timer_);
        state_value->close_on_worker(state_value->abort_reason_ == abort_reason_type::none ? abort_reason_type::closing
                                                                                           : state_value->abort_reason_);
        state_value->connect_in_flight_ = false;
        state_value->close_state_.notify_progress();
        throw;
    }
}

task<void> websocket_client_state::perform_handshake() {
    std::array<std::uint8_t, websocket_client_handshake_nonce_bytes> nonce{};
    if (RAND_bytes_ex(nullptr, nonce.data(), nonce.size(), 0) != 1) {
        throw websocket_client_error(websocket_client_error::code_type::handshake_rejected,
            "failed to generate WebSocket handshake key");
    }

    std::pmr::vector<http_header_view> headers(memory_.resource());
    headers.reserve(config_.headers_.size());
    for (const auto& header : config_.headers_) {
        headers.emplace_back(header.name_, header.value_);
    }
    std::pmr::vector<std::string_view> subprotocols(memory_.resource());
    subprotocols.reserve(config_.subprotocols_.size());
    for (const auto& protocol : config_.subprotocols_) {
        subprotocols.push_back(protocol);
    }
    http1_websocket_client_handshake handshake(
        {.nonce_ = nonce, .headers_ = headers, .subprotocols_ = subprotocols, .user_agent_ = config_.user_agent_, .deflate_ = config_.deflate_},
        memory_.resource());

    std::array<char, max_http_header_bytes + websocket_client_handshake_request_buffer_extra_bytes>
        request_buffer{};
    const auto wire_host = client_uri_host(config_.host_, memory_.resource());
    const auto origin = [&] {
        const http_origin_options origin_options{.host_ = wire_host, .port_ = port()};
        if (config_.scheme_ == websocket_scheme::wss) {
            return http_origin_view::https(origin_options);
        }
        return http_origin_view::http(origin_options);
    }();
    auto prepared_result = handshake.prepare_request(origin, config_.target_, request_buffer);
    const auto* prepared = prepared_result.prepared();
    if (prepared == nullptr) {
        const auto message = prepared_result.failure()
                                 ? std::string(http1_client_request_prepare_error_message(
                                       prepared_result.failure()->error()))
                                 : "WebSocket handshake request head is too large";
        throw websocket_client_error(websocket_client_error::code_type::invalid_config, message);
    }
    http1_client_response_parser parser(prepared->exchange_state(), {.resource_ = memory_.resource()});
    co_await write_transport(prepared->head(), config_.write_timeout_);

    std::array<char, websocket_client_transport_buffer_bytes> bytes_value{};
    for (;;) {
        auto result_value = parser.parse(input_);
        if (const auto* failure = result_value.failure()) {
            throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                std::string(http1_client_response_parse_error_message(failure->error())));
        }
        if (result_value.need_more()) {
            if (input_.size() >= max_http_header_bytes) {
                throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                    "WebSocket handshake response head is too large");
            }
            const auto read = co_await read_transport(bytes_value, config_.connect_timeout_);
            if (read == 0) {
                throw websocket_client_error(websocket_client_error::code_type::io_error,
                    "upstream closed during WebSocket handshake");
            }
            input_.append(bytes_value.data(), read);
            continue;
        }
        auto* parsed_value = result_value.parsed();
        if (parsed_value == nullptr) {
            throw websocket_client_error(websocket_client_error::code_type::handshake_rejected,
                "upstream rejected the WebSocket upgrade");
        }
        if (const auto* informational = parsed_value->plan().informational()) {
            if (informational->persistence() == http1_close_policy::close_after_response) {
                throw websocket_client_error(websocket_client_error::code_type::handshake_rejected,
                    "upstream closed the HTTP exchange before the WebSocket upgrade");
            }
            input_.erase(0, parsed_value->consumed_bytes());
            continue;
        }
        const auto validated = handshake.validate_response(*parsed_value);
        if ((validated.index() != 0)) {
            throw websocket_client_error(websocket_client_error::code_type::handshake_rejected,
                "invalid WebSocket handshake response");
        }
        selected_subprotocol_.assign(std::get<0>(validated).selected_subprotocol_);
        negotiated_compression_ = std::get<0>(validated).compression_;
        input_.erase(0, parsed_value->consumed_bytes());
        co_return;
    }
}

}  // namespace ruvia::detail
