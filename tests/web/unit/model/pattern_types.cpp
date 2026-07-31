#include "test_harness.h"

#include "ruvia/web/detail/model/pattern/PatternTypes.h"

namespace {

using ruvia::detail::model::isPatternDigit;
using ruvia::detail::model::isPatternMeta;
using ruvia::detail::model::isPatternSpace;
using ruvia::detail::model::isPatternWord;

}  // namespace

RUVIA_TEST(pattern_meta_characters) {
    for (const char c : {'^', '$', '[', ']', '(', ')', '{', '}', '|', '+', '*', '?', '.', '\\'}) {
        RUVIA_CHECK(isPatternMeta(c));
    }
    for (const char c : {'a', 'Z', '0', '-', '/', '_', ' ', ':'}) {
        RUVIA_CHECK(!isPatternMeta(c));
    }
}

RUVIA_TEST(pattern_digit_class) {
    RUVIA_CHECK(isPatternDigit('0'));
    RUVIA_CHECK(isPatternDigit('9'));
    RUVIA_CHECK(!isPatternDigit('a'));
    RUVIA_CHECK(!isPatternDigit('/'));  // just below '0'
    RUVIA_CHECK(!isPatternDigit(':'));  // just above '9'
}

RUVIA_TEST(pattern_word_class) {
    for (const char c : {'a', 'z', 'A', 'Z', '0', '9', '_'}) {
        RUVIA_CHECK(isPatternWord(c));
    }
    for (const char c : {'-', '.', '@', ' ', '/'}) {
        RUVIA_CHECK(!isPatternWord(c));
    }
}

RUVIA_TEST(pattern_space_class) {
    for (const char c : {' ', '\t', '\r', '\n', '\f', '\v'}) {
        RUVIA_CHECK(isPatternSpace(c));
    }
    for (const char c : {'a', '0', '-'}) {
        RUVIA_CHECK(!isPatternSpace(c));
    }
}
