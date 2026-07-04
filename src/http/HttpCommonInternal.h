#pragma once

#include "ruvia/http/HttpCommon.h"

namespace ruvia::detail {

struct MultipartPartAccess final {
    [[nodiscard]] static constexpr MultipartPart make(
        std::string_view name,
        std::string_view filename,
        std::string_view contentType,
        std::string_view body) noexcept {
        return MultipartPart(name, filename, contentType, body);
    }
};

struct RequestNameValueViewAccess final {
    [[nodiscard]] static constexpr RequestNameValueView make(
        std::string_view name,
        std::string_view value) noexcept {
        return RequestNameValueView(name, value);
    }
};

}  // namespace ruvia::detail
