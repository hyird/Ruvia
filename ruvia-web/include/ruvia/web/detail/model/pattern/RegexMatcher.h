#pragma once

#include <cstddef>
#include <optional>
#include <regex>
#include <string_view>

#include "ruvia/web/ModelTypes.h"
#include "ruvia/web/detail/model/pattern/PatternTypes.h"

namespace ruvia::detail::model {

// libstdc++ implements std::regex with recursive backtracking whose stack depth
// scales with the number of characters a quantifier matches, so even a benign
// anchored pattern (e.g. "^\\w+$") overflows the worker thread's stack on a long
// enough input -- a SIGSEGV that catching std::regex_error cannot intercept.
// RUVIA_REGEX validates attacker-controlled request fields, so bound the input
// length before matching: an over-long value fails the rule (fail-closed) rather
// than crashing the worker. Fields validated by a format regex are short; longer
// inputs that legitimately need pattern validation should use RUVIA_PATTERN,
// whose engine enforces an explicit step budget (see PatternMatcher.h). This
// does not defend a nested-quantifier pattern the application itself wrote
// (e.g. "^(\\w+)*$") from catastrophic backtracking on a short input -- such a
// pattern is a ReDoS the author must avoid; prefer RUVIA_PATTERN for untrusted
// input.
inline constexpr std::size_t kMaxRegexInputBytes = 4096;

struct RegexPatternState final {
    std::optional<std::regex> regex;

    [[nodiscard]] bool valid() const noexcept {
        return regex.has_value();
    }
};

template <FixedString Pattern>
[[nodiscard]] const RegexPatternState& compiledRegexState() {
    constexpr auto pattern = Pattern.view();
    static const RegexPatternState state = [pattern] {
        RegexPatternState compiled;
        try {
            compiled.regex.emplace(pattern.begin(), pattern.end(),
                std::regex_constants::ECMAScript | std::regex_constants::optimize);
        } catch (const std::regex_error&) {
            compiled.regex.reset();
        }
        return compiled;
    }();
    return state;
}

template <FixedString Pattern>
[[nodiscard]] bool matchRegexPattern(std::string_view value) noexcept {
    if (value.size() > kMaxRegexInputBytes) {
        return false;
    }
    const auto& state = compiledRegexState<Pattern>();
    if (!state.valid()) {
        return false;
    }
    try {
        return std::regex_match(value.begin(), value.end(), *state.regex);
    } catch (const std::regex_error&) {
        return false;
    }
}

}  // namespace ruvia::detail::model
