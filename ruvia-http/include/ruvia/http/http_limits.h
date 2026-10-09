#pragma once

#include <cstddef>

namespace ruvia {

inline constexpr std::size_t max_http_header_bytes = std::size_t{64} * 1024;
inline constexpr std::size_t default_max_buffered_body_bytes = std::size_t{16} * 1024 * 1024;
inline constexpr std::size_t default_max_websocket_message_bytes = std::size_t{16} * 1024 * 1024;
// The parser's built-in body ceiling is a default, not a protocol maximum:
// runtimes configure the real per-request limit through protocol_byte_limit.
inline constexpr std::size_t max_http_request_bytes =
    max_http_header_bytes + default_max_buffered_body_bytes;

}  // namespace ruvia
