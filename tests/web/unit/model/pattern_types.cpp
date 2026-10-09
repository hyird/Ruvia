#include "ruvia/web/detail/model/pattern/pattern_types.h"

#include "test_harness.h"

namespace {

using ruvia::detail::model::is_pattern_digit;
using ruvia::detail::model::is_pattern_meta;
using ruvia::detail::model::is_pattern_space;
using ruvia::detail::model::is_pattern_word;

}  // namespace

RUVIA_TEST(pattern_meta_characters) {
    for (const char c : {'^', '$', '[', ']', '(', ')', '{', '}', '|', '+', '*', '?', '.', '\\'}) {
        RUVIA_CHECK(is_pattern_meta(c));
    }
    for (const char c : {'a', 'Z', '0', '-', '/', '_', ' ', ':'}) {
        RUVIA_CHECK(!is_pattern_meta(c));
    }
}

RUVIA_TEST(pattern_digit_class) {
    RUVIA_CHECK(is_pattern_digit('0'));
    RUVIA_CHECK(is_pattern_digit('9'));
    RUVIA_CHECK(!is_pattern_digit('a'));
    RUVIA_CHECK(!is_pattern_digit('/'));  // just below '0'
    RUVIA_CHECK(!is_pattern_digit(':'));  // just above '9'
}

RUVIA_TEST(pattern_word_class) {
    for (const char c : {'a', 'z', 'A', 'Z', '0', '9', '_'}) {
        RUVIA_CHECK(is_pattern_word(c));
    }
    for (const char c : {'-', '.', '@', ' ', '/'}) {
        RUVIA_CHECK(!is_pattern_word(c));
    }
}

RUVIA_TEST(pattern_space_class) {
    for (const char c : {' ', '\t', '\r', '\n', '\f', '\v'}) {
        RUVIA_CHECK(is_pattern_space(c));
    }
    for (const char c : {'a', '0', '-'}) {
        RUVIA_CHECK(!is_pattern_space(c));
    }
}
