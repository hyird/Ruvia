#include "test_harness.h"

#include <cstddef>
#include <string_view>

#include "ruvia/http/detail/model/PatternMatcher.h"

namespace {

using ruvia::detail::model::matchPatternClass;
using ruvia::detail::model::matchPatternEscape;
using ruvia::detail::model::matchPatternPlan;

}  // namespace

RUVIA_TEST(pattern_match_plan_quantifiers_and_anchoring) {
    // A pattern is fully anchored: the whole value must be consumed.

    // '*' (zero or more).
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^a*$"}>(""));
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^a*$"}>("a"));
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^a*$"}>("aaa"));
    RUVIA_CHECK(!matchPatternPlan<ruvia::FixedString{"^a*$"}>("b"));    // trailing byte unconsumed
    RUVIA_CHECK(!matchPatternPlan<ruvia::FixedString{"^a*$"}>("aab"));

    // '+' (one or more).
    RUVIA_CHECK(!matchPatternPlan<ruvia::FixedString{"^a+$"}>(""));
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^a+$"}>("a"));
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^a+$"}>("aaa"));

    // '?' (zero or one).
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^a?$"}>(""));
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^a?$"}>("a"));
    RUVIA_CHECK(!matchPatternPlan<ruvia::FixedString{"^a?$"}>("aa"));

    // Greedy matching must backtrack so a following atom can still match.
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^a*a$"}>("a"));    // a* takes 0, a takes 1
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^a*a$"}>("aaa"));  // a* backtracks to 2
    RUVIA_CHECK(!matchPatternPlan<ruvia::FixedString{"^a*a$"}>(""));    // no byte for the final atom

    // Escape-class quantifiers and exact-length anchoring.
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^\\d+$"}>("123"));
    RUVIA_CHECK(!matchPatternPlan<ruvia::FixedString{"^\\d+$"}>("12a"));
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^\\d\\d$"}>("12"));
    RUVIA_CHECK(!matchPatternPlan<ruvia::FixedString{"^\\d\\d$"}>("1"));    // too short
    RUVIA_CHECK(!matchPatternPlan<ruvia::FixedString{"^\\d\\d$"}>("123"));  // too long
}

RUVIA_TEST(pattern_match_multiple_quantifiers_backtrack_correctly) {
    // Interleaved quantifiers must backtrack across each other to find a match (or
    // correctly report none). A single-quantifier test cannot reach this multi-atom
    // backtracking. These also guard the match results against a future rewrite of
    // the matcher's search (e.g. memoization to bound worst-case backtracking).

    // Two adjacent greedy stars.
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^a*b*$"}>(""));      // both take zero
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^a*b*$"}>("aaa"));   // b* takes zero
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^a*b*$"}>("bbb"));   // a* takes zero
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^a*b*$"}>("aabb"));
    RUVIA_CHECK(!matchPatternPlan<ruvia::FixedString{"^a*b*$"}>("aba"));  // 'a' after b cannot match
    RUVIA_CHECK(!matchPatternPlan<ruvia::FixedString{"^a*b*$"}>("ba"));   // wrong order

    // Overlapping quantifiers on the same atom: a* must give a byte back to a+.
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^a*a+$"}>("a"));     // a* zero, a+ one
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^a*a+$"}>("aaaa"));
    RUVIA_CHECK(!matchPatternPlan<ruvia::FixedString{"^a*a+$"}>(""));     // a+ needs >= 1

    // A separator between two digit runs -- a realistic multi-quantifier pattern.
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^\\d*-\\d*$"}>("12-34"));
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^\\d*-\\d*$"}>("-"));     // both runs empty
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^\\d*-\\d*$"}>("12-"));
    RUVIA_CHECK(!matchPatternPlan<ruvia::FixedString{"^\\d*-\\d*$"}>("12-3a"));  // trailing non-digit
    RUVIA_CHECK(!matchPatternPlan<ruvia::FixedString{"^\\d*-\\d*$"}>("1234"));   // no separator
}

RUVIA_TEST(pattern_match_escape_classes) {
    RUVIA_CHECK(matchPatternEscape('d', '5'));
    RUVIA_CHECK(!matchPatternEscape('d', 'a'));
    RUVIA_CHECK(matchPatternEscape('w', '_'));   // word includes underscore
    RUVIA_CHECK(matchPatternEscape('w', 'A'));
    RUVIA_CHECK(!matchPatternEscape('w', '-'));
    RUVIA_CHECK(matchPatternEscape('s', ' '));
    RUVIA_CHECK(!matchPatternEscape('s', 'x'));
    // A non-class escape matches the literal character.
    RUVIA_CHECK(matchPatternEscape('.', '.'));
    RUVIA_CHECK(!matchPatternEscape('.', 'x'));
}

RUVIA_TEST(pattern_match_char_class) {
    // A single range.
    RUVIA_CHECK(matchPatternClass("a-z", 0, 3, 'm'));
    RUVIA_CHECK(!matchPatternClass("a-z", 0, 3, '5'));
    // Multiple ranges (a hex digit).
    RUVIA_CHECK(matchPatternClass("0-9a-f", 0, 6, 'c'));
    RUVIA_CHECK(matchPatternClass("0-9a-f", 0, 6, '7'));
    RUVIA_CHECK(!matchPatternClass("0-9a-f", 0, 6, 'z'));
    // A leading '^' negates the class.
    RUVIA_CHECK(matchPatternClass("^0-9", 0, 4, 'a'));
    RUVIA_CHECK(!matchPatternClass("^0-9", 0, 4, '5'));
    // An escape inside the class.
    RUVIA_CHECK(matchPatternClass("\\d", 0, 2, '5'));
    RUVIA_CHECK(!matchPatternClass("\\d", 0, 2, 'a'));
    // An inverted range (first > last) matches nothing.
    RUVIA_CHECK(!matchPatternClass("z-a", 0, 3, 'm'));
    // A range whose end is a backslash is malformed and matches nothing. This is
    // a distinct guard from the inverted-range one: with a low start like 'A'
    // (<= '\\'), first>last does NOT hold, so only the explicit backslash-end
    // check stops "A-\\d" from being misread as the range A(0x41)-\\(0x5C) --
    // which would otherwise wrongly match 'B'.
    RUVIA_CHECK(!matchPatternClass("A-\\d", 0, 4, 'B'));
}
