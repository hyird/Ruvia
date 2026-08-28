#pragma once

#include <cstddef>

namespace ruvia {

inline constexpr std::size_t kMaxHttpHeaderBytes = std::size_t{64} * 1024;
inline constexpr std::size_t kDefaultMaxBufferedBodyBytes = std::size_t{16} * 1024 * 1024;
inline constexpr std::size_t kDefaultMaxWebSocketMessageBytes = std::size_t{16} * 1024 * 1024;
// The parser's built-in body ceiling is a default, not a protocol maximum:
// runtimes configure the real per-request limit through ProtocolByteLimit.
inline constexpr std::size_t kMaxHttpRequestBytes =
    kMaxHttpHeaderBytes + kDefaultMaxBufferedBodyBytes;

}  // namespace ruvia
