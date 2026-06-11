#pragma once

#include <cstddef>

namespace ruvia {

inline constexpr std::size_t kMaxHttpHeaderBytes = 64 * 1024;
inline constexpr std::size_t kDefaultMaxBufferedBodyBytes = 16 * 1024 * 1024;
inline constexpr std::size_t kDefaultMaxStreamBodyBytes = 0;
inline constexpr std::size_t kDefaultMaxWebSocketMessageBytes = 16 * 1024 * 1024;
inline constexpr std::size_t kMaxHttpBodyBytes = kDefaultMaxBufferedBodyBytes;
inline constexpr std::size_t kMaxHttpRequestBytes = kMaxHttpHeaderBytes + kDefaultMaxBufferedBodyBytes;

}  // namespace ruvia
