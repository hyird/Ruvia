#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>

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

[[nodiscard]] inline ValidationTarget validationTargetFromName(std::string_view name) {
    if (name == "json") {
        return ValidationTarget::kJson;
    }
    if (name == "form") {
        return ValidationTarget::kForm;
    }
    if (name == "query") {
        return ValidationTarget::kQuery;
    }
    if (name == "param") {
        return ValidationTarget::kParam;
    }
    if (name == "header") {
        return ValidationTarget::kHeader;
    }
    if (name == "cookie") {
        return ValidationTarget::kCookie;
    }
    throw std::invalid_argument("unknown validation target");
}

}  // namespace ruvia
