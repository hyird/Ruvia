#pragma once

#include <cstdint>

namespace ruvia {

enum class ValidationTarget : std::uint8_t { kJson,
    kForm,
    kQuery,
    kParam,
    kHeader,
    kCookie };

}  // namespace ruvia
