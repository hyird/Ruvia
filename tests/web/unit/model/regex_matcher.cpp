#include "ruvia/web/detail/model/pattern/regex_matcher.h"

#include <cstddef>
#include <string>

#include "test_harness.h"

namespace {

using ruvia::detail::model::match_regex_pattern;
using ruvia::detail::model::max_regex_input_bytes;

}  // namespace

RUVIA_TEST(regex_matcher_matches_the_bounded_pattern_dialect) {
    RUVIA_CHECK(match_regex_pattern<ruvia::fixed_string{"^\\w+$"}>("abc123"));
    RUVIA_CHECK(!match_regex_pattern<ruvia::fixed_string{"^\\w+$"}>("has space"));
    RUVIA_CHECK(match_regex_pattern<ruvia::fixed_string{"^[a-z]+\\d?$"}>("letters7"));
    RUVIA_CHECK(!match_regex_pattern<ruvia::fixed_string{"^[a-z]+\\d?$"}>("Letters77"));
}

RUVIA_TEST(regex_matcher_accepts_input_at_the_cap) {
    const std::string at_cap(max_regex_input_bytes, 'a');
    RUVIA_CHECK(match_regex_pattern<ruvia::fixed_string{"^\\w+$"}>(at_cap));
}

RUVIA_TEST(regex_matcher_rejects_oversized_patterns) {
    std::string large_pattern(257, 'a');
    large_pattern.front() = '^';
    large_pattern.back() = '$';
    RUVIA_CHECK(!ruvia::detail::model::compile_pattern_plan<257>(large_pattern).valid_);
}

RUVIA_TEST(regex_matcher_rejects_input_past_the_cap) {
    const std::string over_cap(max_regex_input_bytes + 1, 'a');
    RUVIA_CHECK(!match_regex_pattern<ruvia::fixed_string{"^\\w+$"}>(over_cap));
}

RUVIA_TEST(regex_matcher_fails_closed_when_the_step_budget_is_exhausted) {
    // This short pattern has overlapping greedy quantifiers; a modest input is
    // enough to force combinatorial backtracking beyond the shared work budget.
    const std::string adversarial(64, 'a');
    RUVIA_CHECK(!match_regex_pattern<ruvia::fixed_string{"^a*a*a*a*a*a*a*a*b$"}>(adversarial));
}
