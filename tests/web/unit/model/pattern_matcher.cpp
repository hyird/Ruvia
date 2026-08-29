#include "test_harness.h"

#include <cstddef>
#include <string_view>

#include "ruvia/web/detail/model/pattern/PatternMatcher.h"

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
    RUVIA_CHECK(!matchPatternPlan<ruvia::FixedString{"^a*$"}>("b"));  // trailing byte unconsumed
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
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^a*b*$"}>(""));     // both take zero
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^a*b*$"}>("aaa"));  // b* takes zero
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^a*b*$"}>("bbb"));  // a* takes zero
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^a*b*$"}>("aabb"));
    RUVIA_CHECK(
        !matchPatternPlan<ruvia::FixedString{"^a*b*$"}>("aba"));         // 'a' after b cannot match
    RUVIA_CHECK(!matchPatternPlan<ruvia::FixedString{"^a*b*$"}>("ba"));  // wrong order

    // Overlapping quantifiers on the same atom: a* must give a byte back to a+.
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^a*a+$"}>("a"));  // a* zero, a+ one
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^a*a+$"}>("aaaa"));
    RUVIA_CHECK(!matchPatternPlan<ruvia::FixedString{"^a*a+$"}>(""));  // a+ needs >= 1

    // A separator between two digit runs -- a realistic multi-quantifier pattern.
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^\\d*-\\d*$"}>("12-34"));
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^\\d*-\\d*$"}>("-"));  // both runs empty
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^\\d*-\\d*$"}>("12-"));
    RUVIA_CHECK(
        !matchPatternPlan<ruvia::FixedString{"^\\d*-\\d*$"}>("12-3a"));         // trailing non-digit
    RUVIA_CHECK(!matchPatternPlan<ruvia::FixedString{"^\\d*-\\d*$"}>("1234"));  // no separator
}

RUVIA_TEST(pattern_match_escape_classes) {
    RUVIA_CHECK(matchPatternEscape('d', '5'));
    RUVIA_CHECK(!matchPatternEscape('d', 'a'));
    RUVIA_CHECK(matchPatternEscape('w', '_'));  // word includes underscore
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
    // matchPatternClass does NOT interpret negation: the compiler strips a class's
    // leading '[^' and records it on the atom (see matchPatternAtom), so here a '^'
    // in the body is an ordinary literal member. The class "^0-9" matches '^' or a
    // digit; 'a' matches neither.
    RUVIA_CHECK(matchPatternClass("^0-9", 0, 4, '^'));
    RUVIA_CHECK(matchPatternClass("^0-9", 0, 4, '5'));
    RUVIA_CHECK(!matchPatternClass("^0-9", 0, 4, 'a'));
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

RUVIA_TEST(pattern_match_negated_class_with_caret_member) {
    // Through the real compile+match pipeline. The negation '^' of a class is
    // consumed by the compiler (which sets the atom's negate flag); a SECOND '^'
    // is an ordinary literal member. So [^^] means "any char except '^'" and must
    // not be double-negated into "matches nothing".
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^[^^]$"}>("x"));    // 'x' is not '^'
    RUVIA_CHECK(!matchPatternPlan<ruvia::FixedString{"^[^^]$"}>("^"));   // '^' is excluded
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^[^^a]$"}>("b"));   // not '^' and not 'a'
    RUVIA_CHECK(!matchPatternPlan<ruvia::FixedString{"^[^^a]$"}>("a"));  // 'a' is excluded
    RUVIA_CHECK(!matchPatternPlan<ruvia::FixedString{"^[^^a]$"}>("^"));  // '^' is excluded

    // A caret as a non-first literal member of a positive class stays literal.
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^[a^]$"}>("^"));
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^[a^]$"}>("a"));
    RUVIA_CHECK(!matchPatternPlan<ruvia::FixedString{"^[a^]$"}>("b"));

    // Sanity: an ordinary negated class is unaffected.
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^[^0-9]$"}>("a"));
    RUVIA_CHECK(!matchPatternPlan<ruvia::FixedString{"^[^0-9]$"}>("5"));
}

RUVIA_TEST(pattern_match_escaped_literals_are_not_meta) {
    // A backslash-escaped metacharacter matches that byte literally, not its
    // special meaning -- '\.' is a literal dot, NOT "any char".
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^\\.$"}>("."));
    RUVIA_CHECK(!matchPatternPlan<ruvia::FixedString{"^\\.$"}>("a"));
    // '\*' is a literal asterisk, not a quantifier.
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^\\*$"}>("*"));
    RUVIA_CHECK(!matchPatternPlan<ruvia::FixedString{"^\\*$"}>("x"));
    // An escaped ']' inside a class is a literal member (the only way to put a ']'
    // in a class, since an unescaped ']' closes it): [\]] matches ']' and nothing else.
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^[\\]]$"}>("]"));
    RUVIA_CHECK(!matchPatternPlan<ruvia::FixedString{"^[\\]]$"}>("a"));
}

RUVIA_TEST(pattern_match_bounds_catastrophic_backtracking) {
    // Adjacent unanchored quantifiers backtrack combinatorially (~C(n+k, k)) when the
    // tail fails: distributing n input chars among k "*" atoms. The step budget caps
    // this so hostile input cannot hang the matcher (ReDoS defence). The match fails
    // fast rather than exploring the space; without the budget this call would not
    // return promptly.
    const std::string hostile(50, 'a');  // 50 'a's then no 'b' -> full backtrack
    RUVIA_CHECK(!matchPatternPlan<ruvia::FixedString{"^a*a*a*a*a*a*a*a*b$"}>(hostile));
    // A genuinely matching value under the same pattern still matches (budget is huge).
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^a*a*a*a*a*a*a*a*b$"}>("aaab"));
}

RUVIA_TEST(pattern_match_bounds_greedy_rescan_of_fixed_atom) {
    // A variable quantifier followed by a fixed atom it also matches -- "^a*a$",
    // "^\d*\d$" -- previously scanned the full matching run and then discarded all
    // but the one character the fixed atom keeps, repeating that O(L) scan at every
    // backtrack position: O(n^2) work the per-recursion step budget never charges
    // for, hanging on large input. The scan is now capped at what the quantifier can
    // consume, so a hostile input fails fast. 100k chars would take tens of seconds
    // under the old O(n^2) path; here it must return promptly.
    const std::string manyA(100000, 'a');
    RUVIA_CHECK(!matchPatternPlan<ruvia::FixedString{"^a*a$"}>(manyA + "b"));
    const std::string manyDigits(100000, '5');
    RUVIA_CHECK(!matchPatternPlan<ruvia::FixedString{"^\\d*\\d$"}>(manyDigits + "x"));

    // Matching semantics for legitimate inputs are unchanged.
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^a*a$"}>("a"));
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^a*a$"}>("aaa"));
    RUVIA_CHECK(!matchPatternPlan<ruvia::FixedString{"^a*a$"}>("b"));
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^\\d*\\d$"}>("5"));
    RUVIA_CHECK(matchPatternPlan<ruvia::FixedString{"^\\d*\\d$"}>("123"));
}
