#pragma once

#include <cstddef>
#include <limits>

namespace ruvia::detail {

constexpr std::size_t websocket_client_handshake_nonce_bytes = 16;
constexpr std::size_t websocket_client_handshake_request_buffer_extra_bytes = 1024;
constexpr std::size_t websocket_client_transport_buffer_bytes = std::size_t{16} * 1024;

}  // namespace ruvia::detail
