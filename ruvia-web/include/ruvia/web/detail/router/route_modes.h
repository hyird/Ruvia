#pragma once

#include <cstdint>

namespace ruvia::detail {

enum class request_body_mode : std::uint8_t { buffered,
    stream };

}  // namespace ruvia::detail
