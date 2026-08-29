#pragma once

#include <cstdint>

namespace ruvia::detail {

enum class RequestBodyMode : std::uint8_t { kBuffered,
    kStream };

}  // namespace ruvia::detail
