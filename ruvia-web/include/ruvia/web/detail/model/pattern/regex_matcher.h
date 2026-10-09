#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/web/detail/model/pattern/pattern_compiler.h"
#include "ruvia/web/detail/model/pattern/pattern_matcher.h"
#include "ruvia/web/model_types.h"

namespace ruvia::detail::model {

// RUVIA_REGEX retains its historical full-match use, but deliberately accepts
// only the bounded RUVIA_PATTERN dialect. General std::regex is not safe for
// attacker-controlled input because it may backtrack without bound. Unsupported
// syntax is rejected at compile time; patterns are capped at 256 bytes to bound
// compiler storage and matcher recursion. Inputs are capped at 4096 bytes, and
// matching charges scanned bytes to the shared work budget; either limit fails the
// validation rule closed.
inline constexpr std::size_t max_regex_input_bytes = 4096;

template <fixed_string pattern>
struct compiled_regex_plan final {
    static constexpr std::size_t capacity =
        pattern.view().size() < max_pattern_bytes ? pattern.view().size() : max_pattern_bytes;
    static constexpr auto value = compile_pattern_plan<capacity>(pattern.view());
    static_assert(value.valid_,
        "RUVIA_REGEX supports only anchored patterns using literals, '.', character classes, \\d, \\w, \\s, and '*', '+', '?'. "
        "General std::regex syntax is unsupported; use RUVIA_CUSTOM for other matching logic.");
};

template <fixed_string pattern>
[[nodiscard]] constexpr bool match_regex_pattern(std::string_view value) noexcept {
    if (value.size() > max_regex_input_bytes) {
        return false;
    }
    constexpr auto plan = compiled_regex_plan<pattern>::value;
    std::size_t budget = max_pattern_match_steps;
    return match_pattern_plan_from(plan, pattern.view(), value, 0, 0, budget);
}

}  // namespace ruvia::detail::model
