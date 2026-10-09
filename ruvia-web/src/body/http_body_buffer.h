#pragma once

#include <cstddef>

namespace ruvia::detail {

inline constexpr std::size_t http_body_buffer_bytes = std::size_t{8} * 1024;

}  // namespace ruvia::detail
