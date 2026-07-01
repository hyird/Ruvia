#pragma once

#include <cstdint>

namespace ruvia {

enum class ValidationTarget : std::uint8_t {
    kJson,
    kForm,
    kQuery,
    kParam,
    kHeader,
    kCookie
};

inline constexpr ValidationTarget Json = ValidationTarget::kJson;
inline constexpr ValidationTarget Form = ValidationTarget::kForm;
inline constexpr ValidationTarget Query = ValidationTarget::kQuery;
inline constexpr ValidationTarget Param = ValidationTarget::kParam;
inline constexpr ValidationTarget Header = ValidationTarget::kHeader;
inline constexpr ValidationTarget Cookie = ValidationTarget::kCookie;

}  // namespace ruvia
