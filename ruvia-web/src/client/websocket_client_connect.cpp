#include <array>
#include <cstdint>
#include <exception>
#include <span>
#include <string>
#include <utility>
#include <variant>

#include <openssl/rand.h>

#include "ruvia/core/operation_timeout.h"
#include "ruvia/http/http1_client_request_writer.h"
#include "ruvia/http/http1_client_response_parser.h"
#include "ruvia/http/http1_close_policy.h"
#include "ruvia/http/http1_websocket_client_handshake.h"
#include "ruvia/http/http_io_buffer.h"
#include "ruvia/http/http_limits.h"

#include "client/http1_request_head.h"
#include "client/websocket_client_internal.h"
#include "client/websocket_client_state.h"

namespace ruvia::detail {

task<void> websocket_client_state::connect() {
    // One deadline bounds transport setup, the opening handshake and protocol
    // setup.
    arm(connect_timer_, config_.connect_timeout_, client_abort_reason::timeout);
    std::exception_ptr failure;
    try {
        co_await establish();
    } catch (...) {
        failure = std::current_exception();
    }
    disarm(connect_timer_);
    if (failure == nullptr) {
        co_return;
    }
    // A failed connect closes its transport before publishing the error.
    close_now();
    std::rethrow_exception(failure);
}

task<void> websocket_client_state::establish() {
    const operation_timeout unbounded(std::nullopt);
    const auto result = co_await transport_.connect(unbounded);
    if (!result.ok()) {
        throw_transport_failure(result);
    }
    if (!transport_.alpn_allows_http11()) {
        throw websocket_client_error(websocket_client_error::code_type::handshake_rejected,
            "upstream negotiated an unsupported WebSocket HTTP protocol");
    }
    co_await perform_handshake();
    protocol_.emplace(websocket_connection_options{
        .resource_ = memory_.resource(),
        .message_limit_ = protocol_byte_limit::limited(config_.max_message_bytes_),
        .compression_ = negotiated_compression_,
        .role_ = websocket_connection_role::client,
        .mask_key_generator_ = &websocket_client_state::generate_mask,
        .compression_level_ = config_.compression_level_,
        .max_buffered_input_bytes_ = websocket_client_max_buffered_input_bytes(config_.max_message_bytes_)});
    feed_input(input_);
    // From here input_ is the transport read buffer, sized once per connection.
    input_.clear();
    input_.resize(websocket_client_transport_buffer_bytes);
    last_active_ms_ = steady_now_ms();
    liveness_state_ = websocket_liveness_idle{};
    if (config_.heartbeat_.ping_interval_.has_value()) {
        arm_heartbeat_timer(*config_.heartbeat_.ping_interval_);
    }
}

task<void> websocket_client_state::perform_handshake() {
    std::array<std::uint8_t, websocket_client_handshake_nonce_bytes> nonce{};
    if (RAND_bytes_ex(nullptr, nonce.data(), nonce.size(), 0) != 1) {
        throw websocket_client_error(websocket_client_error::code_type::handshake_rejected,
            "failed to generate WebSocket handshake key");
    }

    http1_websocket_client_handshake handshake({.nonce_ = nonce,
                                                   .headers_ = config_.header_views_,
                                                   .subprotocols_ = config_.subprotocol_views_,
                                                   .user_agent_ = config_.user_agent_,
                                                   .deflate_ = config_.deflate_},
        memory_.resource());

    // Uninitialized worker storage, released when the handshake ends: the
    // frame stays small and no buffer is zero-filled.
    constexpr std::size_t max_request_head_bytes =
        max_http_header_bytes + websocket_client_handshake_request_buffer_extra_bytes;
    http_io_buffer request_buffer(memory_.resource());
    auto prepared_result = prepare_http1_request_head(request_buffer, max_request_head_bytes,
        [&](std::span<char> buffer) { return handshake.prepare_request(config_.origin_, config_.target_, buffer); });
    const auto* prepared = prepared_result.prepared();
    if (prepared == nullptr) {
        const auto message = prepared_result.failure()
                                 ? std::string(http1_client_request_prepare_error_message(
                                       prepared_result.failure()->error()))
                                 : "WebSocket handshake request head is too large";
        throw websocket_client_error(websocket_client_error::code_type::invalid_config, message);
    }
    http1_client_response_parser parser(prepared->exchange_state(), {.resource_ = memory_.resource()});
    co_await write_transport(prepared->head());

    // The response head is read into the request buffer's storage.
    request_buffer.clear();
    const auto bytes_value = request_buffer.prepare(websocket_client_transport_buffer_bytes);
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
            const auto read = co_await read_transport(bytes_value, std::nullopt);
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
