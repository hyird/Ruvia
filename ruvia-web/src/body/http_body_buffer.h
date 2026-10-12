#pragma once

#include <cstddef>

namespace ruvia::detail {

inline constexpr std::size_t http_body_buffer_bytes = std::size_t{8} * 1024;
// The smallest capacity a buffered known-length body grows to while it is read
// straight into its own storage.
inline constexpr std::size_t known_length_min_read_bytes = std::size_t{16} * 1024;

}  // namespace ruvia::detail
