#include "test_harness.h"

#include <cstddef>
#include <string>

#include "ruvia/web/detail/model/pattern/RegexMatcher.h"

namespace {

using ruvia::detail::model::kMaxRegexInputBytes;
using ruvia::detail::model::matchRegexPattern;

}  // namespace

RUVIA_TEST(regex_matcher_matches_values_within_the_input_cap) {
    RUVIA_CHECK(matchRegexPattern<ruvia::FixedString{"^\\w+$"}>("abc123"));
    RUVIA_CHECK(!matchRegexPattern<ruvia::FixedString{"^\\w+$"}>("has space"));

    // A value exactly at the cap is still matched normally.
    const std::string atCap(kMaxRegexInputBytes, 'a');
    RUVIA_CHECK(matchRegexPattern<ruvia::FixedString{"^\\w+$"}>(atCap));
}

RUVIA_TEST(regex_matcher_rejects_input_past_the_cap_without_matching) {
    // One byte over the cap. This all-word-char value WOULD match "^\\w+$", but
    // the length guard rejects it before std::regex_match so an adversarial long
    // input cannot drive libstdc++'s recursive backtracking into a stack overflow.
    const std::string overCap(kMaxRegexInputBytes + 1, 'a');
    RUVIA_CHECK(!matchRegexPattern<ruvia::FixedString{"^\\w+$"}>(overCap));
}
