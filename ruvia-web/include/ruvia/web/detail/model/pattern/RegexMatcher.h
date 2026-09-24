#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/web/ModelTypes.h"
#include "ruvia/web/detail/model/pattern/PatternCompiler.h"
#include "ruvia/web/detail/model/pattern/PatternMatcher.h"

namespace ruvia::detail::model {

// RUVIA_REGEX retains its historical full-match use, but deliberately accepts
// only the bounded RUVIA_PATTERN dialect. General std::regex is not safe for
// attacker-controlled input because it may backtrack without bound. Unsupported
// syntax is rejected at compile time; patterns are capped at 256 bytes to bound
// compiler storage and matcher recursion. Inputs are capped at 4096 bytes, and
// matching charges scanned bytes to the shared work budget; either limit fails the
// validation rule closed.
inline constexpr std::size_t kMaxRegexInputBytes = 4096;

template <FixedString Pattern>
struct CompiledRegexPlan final {
    static constexpr std::size_t capacity =
        Pattern.view().size() < kMaxPatternBytes ? Pattern.view().size() : kMaxPatternBytes;
    static constexpr auto value = compilePatternPlan<capacity>(Pattern.view());
    static_assert(value.valid,
        "RUVIA_REGEX supports only anchored patterns using literals, '.', character classes, \\d, \\w, \\s, and '*', '+', '?'. "
        "General std::regex syntax is unsupported; use RUVIA_CUSTOM for other matching logic.");
};

template <FixedString Pattern>
[[nodiscard]] constexpr bool matchRegexPattern(std::string_view value) noexcept {
    if (value.size() > kMaxRegexInputBytes) {
        return false;
    }
    constexpr auto plan = CompiledRegexPlan<Pattern>::value;
    std::size_t budget = kMaxPatternMatchSteps;
    return matchPatternPlanFrom(plan, Pattern.view(), value, 0, 0, budget);
}

}  // namespace ruvia::detail::model
