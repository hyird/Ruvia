#pragma once

#include <cstddef>

namespace ruvia::detail {

inline constexpr std::size_t http_body_buffer_bytes = std::size_t{8} * 1024;
// The smallest capacity a buffered known-length body grows to while it is read
// straight into its own storage. It matches the WebSocket read window: a peer
// that sends only headers can make the declared length allocate at most this
// much before any body bytes arrive; beyond it capacity doubles from arrivals.
inline constexpr std::size_t known_length_min_read_bytes = std::size_t{64} * 1024;

}  // namespace ruvia::detail
