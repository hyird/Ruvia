#pragma once

#include <string_view>

namespace ruvia::detail {

// Trim RFC 7230 section 3.2.3 optional whitespace (OWS = *( SP / HTAB )) from both ends.
// Sole owner shared by header-token utilities and model parsing so the two
// layers cannot diverge on what counts as trimmable whitespace.
[[nodiscard]] inline std::string_view httpTrimOws(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

}  // namespace ruvia::detail
