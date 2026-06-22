#pragma once

#include <optional>
#include <regex>
#include <string_view>

#include "ruvia/http/detail/model/PatternTypes.h"

namespace ruvia::detail::model {

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
            compiled.regex.emplace(
                pattern.begin(),
                pattern.end(),
                std::regex_constants::ECMAScript | std::regex_constants::optimize);
        } catch (const std::regex_error&) {
        }
        return compiled;
    }();
    return state;
}

template <FixedString Pattern>
[[nodiscard]] bool matchRegexPattern(std::string_view value) noexcept {
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
