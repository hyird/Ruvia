#include "test_harness.h"

#include <string_view>

#include "ruvia/web/detail/model/pattern/PatternCompiler.h"

namespace {

using ruvia::detail::model::compilePatternPlan;

// The compiler requires an anchored ^...$ pattern and validates the atom grammar
// (char classes, \d/\w/\s, '.', literals, and *,+,? quantifiers).
[[nodiscard]] bool compiles(std::string_view pattern) noexcept {
    return compilePatternPlan<32>(pattern).valid;
}

}  // namespace

RUVIA_TEST(pattern_compile_accepts_wellformed) {
    RUVIA_CHECK(compiles("^\\d+$"));
    RUVIA_CHECK(compiles("^[a-z]+$"));
    RUVIA_CHECK(compiles("^[^0-9]$"));  // negated class
    RUVIA_CHECK(compiles("^.*$"));
    RUVIA_CHECK(compiles("^abc$"));
    RUVIA_CHECK(compiles("^\\w+\\s\\d?$"));  // \w \s \d with quantifiers
}

RUVIA_TEST(pattern_compile_requires_both_anchors) {
    RUVIA_CHECK(!compiles("\\d+"));   // no anchors
    RUVIA_CHECK(!compiles("^\\d+"));  // missing trailing $
    RUVIA_CHECK(!compiles("\\d+$"));  // missing leading ^
    RUVIA_CHECK(!compiles(""));       // empty
    RUVIA_CHECK(!compiles("^"));      // single char, too short
}

RUVIA_TEST(pattern_compile_rejects_malformed_atoms) {
    RUVIA_CHECK(!compiles("^[a-z$"));  // unterminated char class
    RUVIA_CHECK(!compiles("^a**$"));   // stacked quantifiers
    RUVIA_CHECK(!compiles("^+$"));     // quantifier with no preceding atom
    RUVIA_CHECK(!compiles("^a\\$"));   // dangling backslash: '\' escapes the '$' anchor, none left to close
}

RUVIA_TEST(pattern_compile_escape_handling) {
    // Escaped metacharacters are well-formed literal atoms.
    RUVIA_CHECK(compiles("^\\.$"));  // literal dot
    RUVIA_CHECK(compiles("^\\*$"));  // literal asterisk
    RUVIA_CHECK(compiles("^\\[$"));  // literal '['
    // A class may carry an escaped ']' as a member, alone or amid other members.
    RUVIA_CHECK(compiles("^[\\]]$"));
    RUVIA_CHECK(compiles("^[a\\]b]$"));
}
