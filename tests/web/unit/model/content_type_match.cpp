#include "test_harness.h"

#include <string_view>

#include "ruvia/web/detail/model/parse/Parser.h"

namespace {

using ruvia::detail::contentTypeMatches;
using ruvia::detail::httpAsciiEqualsIgnoreCase;

}  // namespace

RUVIA_TEST(model_ascii_equals_ignore_case_folds_only_letters) {
    RUVIA_CHECK(httpAsciiEqualsIgnoreCase("Text/HTML", "text/html"));
    RUVIA_CHECK(httpAsciiEqualsIgnoreCase("", ""));
    RUVIA_CHECK(!httpAsciiEqualsIgnoreCase("abc", "abcd"));  // length mismatch
    RUVIA_CHECK(!httpAsciiEqualsIgnoreCase("abc", "abd"));

    // Only A-Z fold. A naive `| 0x20` fold would wrongly equate the punctuation
    // adjacent to the letter ranges ('[' 0x5B with '{' 0x7B, '@' 0x40 with '`').
    RUVIA_CHECK(!httpAsciiEqualsIgnoreCase("[", "{"));
    RUVIA_CHECK(!httpAsciiEqualsIgnoreCase("@", "`"));

    // High (non-ASCII) bytes are compared exactly, never folded.
    RUVIA_CHECK(httpAsciiEqualsIgnoreCase(
        std::string_view("\xC3\xA9", 2), std::string_view("\xC3\xA9", 2)));
    RUVIA_CHECK(!httpAsciiEqualsIgnoreCase(
        std::string_view("\xC3\xA9", 2), std::string_view("\xC3\x89", 2)));
}

RUVIA_TEST(content_type_matches_ignoring_parameters) {
    RUVIA_CHECK(contentTypeMatches("application/json", "application/json"));
    // Media-type parameters after ';' are ignored.
    RUVIA_CHECK(contentTypeMatches("application/json; charset=utf-8", "application/json"));
    // OWS around the media type is trimmed.
    RUVIA_CHECK(contentTypeMatches("application/json ; charset=utf-8", "application/json"));
    // The media type is matched case-insensitively.
    RUVIA_CHECK(contentTypeMatches("APPLICATION/JSON", "application/json"));
    RUVIA_CHECK(contentTypeMatches(
        "application/x-www-form-urlencoded", "application/x-www-form-urlencoded"));
}

RUVIA_TEST(content_type_matches_rejects_mismatches) {
    RUVIA_CHECK(!contentTypeMatches("text/html", "application/json"));
    RUVIA_CHECK(!contentTypeMatches("", "application/json"));  // empty content type
    RUVIA_CHECK(!contentTypeMatches("application/jsonx", "application/json"));  // exact, not prefix
    RUVIA_CHECK(!contentTypeMatches("application/json", "application/jsonx"));
}
