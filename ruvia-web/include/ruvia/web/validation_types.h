#pragma once

#include <cstdint>

namespace ruvia {

enum class validation_target : std::uint8_t { json,
    form,
    query,
    param,
    header,
    cookie };

}  // namespace ruvia
