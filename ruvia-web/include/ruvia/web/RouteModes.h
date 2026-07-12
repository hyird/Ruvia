#pragma once

#include <cstddef>

namespace ruvia {

inline constexpr std::size_t kMaxRouteParams = 16;

namespace detail {

enum class RequestBodyMode {
    kBuffered,
    kStream
};

}  // namespace detail

}  // namespace ruvia
