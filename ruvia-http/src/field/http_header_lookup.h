#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "ruvia/http/http_ascii.h"

namespace ruvia::detail {

// Repeated fields retain their wire order; singular convenience accessors
// select the last matching field without copying its borrowed value.
template <typename field_type>
[[nodiscard]] std::optional<std::string_view> find_last_http_header(
    std::span<const field_type> fields, std::string_view name) noexcept {
    for (auto field = fields.rbegin(); field != fields.rend(); ++field) {
        if (http_ascii_equals_ignore_case(field->name(), name)) {
            return field->value();
        }
    }
    return std::nullopt;
}

}  // namespace ruvia::detail
