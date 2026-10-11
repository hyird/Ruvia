#pragma once

#include <cstdint>

namespace ruvia {

enum class http_scheme : std::uint8_t {
    http,
    https,
};

}  // namespace ruvia
