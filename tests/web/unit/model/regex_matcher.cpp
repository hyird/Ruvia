#include <cstddef>
#include <string>

#include "ruvia/web/detail/model/pattern/RegexMatcher.h"

#include "test_harness.h"

namespace {

using ruvia::detail::model::kMaxRegexInputBytes;
using ruvia::detail::model::matchRegexPattern;

}  // namespace

RUVIA_TEST(regex_matcher_matches_the_bounded_pattern_dialect) {
    RUVIA_CHECK(matchRegexPattern<ruvia::FixedString{"^\\w+$"}>("abc123"));
    RUVIA_CHECK(!matchRegexPattern<ruvia::FixedString{"^\\w+$"}>("has space"));
    RUVIA_CHECK(matchRegexPattern<ruvia::FixedString{"^[a-z]+\\d?$"}>("letters7"));
    RUVIA_CHECK(!matchRegexPattern<ruvia::FixedString{"^[a-z]+\\d?$"}>("Letters77"));
}

RUVIA_TEST(regex_matcher_accepts_input_at_the_cap) {
    const std::string atCap(kMaxRegexInputBytes, 'a');
    RUVIA_CHECK(matchRegexPattern<ruvia::FixedString{"^\\w+$"}>(atCap));
}

RUVIA_TEST(regex_matcher_rejects_oversized_patterns) {
    std::string largePattern(257, 'a');
    largePattern.front() = '^';
    largePattern.back() = '$';
    RUVIA_CHECK(!ruvia::detail::model::compilePatternPlan<257>(largePattern).valid);
}

RUVIA_TEST(regex_matcher_rejects_input_past_the_cap) {
    const std::string overCap(kMaxRegexInputBytes + 1, 'a');
    RUVIA_CHECK(!matchRegexPattern<ruvia::FixedString{"^\\w+$"}>(overCap));
}

RUVIA_TEST(regex_matcher_fails_closed_when_the_step_budget_is_exhausted) {
    // This short pattern has overlapping greedy quantifiers; a modest input is
    // enough to force combinatorial backtracking beyond the shared work budget.
    const std::string adversarial(64, 'a');
    RUVIA_CHECK(!matchRegexPattern<ruvia::FixedString{"^a*a*a*a*a*a*a*a*b$"}>(adversarial));
}
