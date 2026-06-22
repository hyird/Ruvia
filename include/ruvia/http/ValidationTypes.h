#pragma once

#include <cstdint>

namespace ruvia {

enum class ValidationTarget : std::uint8_t {
    kJson,
    kForm
};

inline constexpr ValidationTarget Json = ValidationTarget::kJson;
inline constexpr ValidationTarget Form = ValidationTarget::kForm;

}  // namespace ruvia
