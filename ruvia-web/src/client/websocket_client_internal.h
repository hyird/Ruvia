#pragma once

#include <cstddef>
#include <memory>

#include "ruvia/web/websocket_client.h"

#include "client/websocket_client_state.h"

namespace ruvia::detail {

constexpr std::size_t websocket_client_handshake_nonce_bytes = 16;
constexpr std::size_t websocket_client_handshake_request_buffer_extra_bytes = 1024;
constexpr std::size_t websocket_client_transport_buffer_bytes = std::size_t{16} * 1024;
constexpr std::size_t websocket_client_close_handshake_buffer_bytes = std::size_t{4} * 1024;

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
