#pragma once

#include <cstddef>
#include <limits>
#include <memory>

#include "ruvia/web/websocket_client.h"

#include "client/websocket_client_state.h"

namespace ruvia::detail {

constexpr std::size_t websocket_client_handshake_nonce_bytes = 16;
constexpr std::size_t websocket_client_handshake_request_buffer_extra_bytes = 1024;
constexpr std::size_t websocket_client_transport_buffer_bytes = std::size_t{16} * 1024;
constexpr std::size_t websocket_client_close_handshake_buffer_bytes = std::size_t{4} * 1024;
// RFC 6455 Section 5.2: base header, 64-bit extended length and masking key.
constexpr std::size_t websocket_client_max_frame_header_bytes = 14;

// The client feeds one transport read only after next_event() needs input, so
// the protocol holds at most one incomplete frame within the configured
// message limit plus that read. Saturate instead of wrapping for huge limits.
[[nodiscard]] constexpr std::size_t websocket_client_max_buffered_input_bytes(
    std::size_t max_message_bytes) noexcept {
    constexpr auto margin = websocket_client_max_frame_header_bytes + websocket_client_transport_buffer_bytes;
    constexpr auto max_value = (std::numeric_limits<std::size_t>::max)();
    return max_message_bytes > max_value - margin ? max_value : max_message_bytes + margin;
}

[[nodiscard]] inline websocket_client_error::code_type websocket_client_transport_error_code(
    bool secure) noexcept {
    return secure ? websocket_client_error::code_type::tls_failed : websocket_client_error::code_type::io_error;
}

struct websocket_client_stop_abort final {
    std::weak_ptr<websocket_client_state> state_;

    void operator()() noexcept {
        if (const auto owner = state_.lock()) {
            owner->request_cancel();
        }
    }
};

}  // namespace ruvia::detail
