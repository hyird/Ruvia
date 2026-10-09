#include <string_view>

#include "ruvia/web/detail/model/parse/parser.h"

#include "test_harness.h"

namespace {

using ruvia::detail::content_type_matches;

}  // namespace

RUVIA_TEST(content_type_matches_ignoring_parameters) {
    RUVIA_CHECK(content_type_matches("application/json", "application/json"));
    // Media-type parameters after ';' are ignored.
    RUVIA_CHECK(content_type_matches("application/json; charset=utf-8", "application/json"));
    // OWS around the media type is trimmed.
    RUVIA_CHECK(content_type_matches("application/json ; charset=utf-8", "application/json"));
    // The media type is matched case-insensitively.
    RUVIA_CHECK(content_type_matches("APPLICATION/JSON", "application/json"));
    RUVIA_CHECK(content_type_matches(
        "application/x-www-form-urlencoded", "application/x-www-form-urlencoded"));
}

RUVIA_TEST(content_type_matches_rejects_mismatches) {
    RUVIA_CHECK(!content_type_matches("text/html", "application/json"));
    RUVIA_CHECK(!content_type_matches("", "application/json"));                   // empty content type
    RUVIA_CHECK(!content_type_matches("application/jsonx", "application/json"));  // exact, not prefix
    RUVIA_CHECK(!content_type_matches("application/json", "application/jsonx"));
}
