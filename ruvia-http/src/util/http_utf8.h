#pragma once

#include <string_view>

namespace ruvia::detail {

// Whether `value` is well-formed UTF-8 (RFC 3629): no overlong forms,
// surrogates, or code points above U+10FFFF.
[[nodiscard]] bool is_valid_utf8(std::string_view value) noexcept;

}  // namespace ruvia::detail
