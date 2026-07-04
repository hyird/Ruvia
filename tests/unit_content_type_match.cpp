#include "test_harness.h"

#include <string_view>

#include "ruvia/http/detail/model/Parser.h"

namespace {

using ruvia::detail::contentTypeMatches;

}  // namespace

RUVIA_TEST(content_type_matches_ignoring_parameters) {
    RUVIA_CHECK(contentTypeMatches("application/json", "application/json"));
    // Media-type parameters after ';' are ignored.
    RUVIA_CHECK(contentTypeMatches("application/json; charset=utf-8", "application/json"));
    // OWS around the media type is trimmed.
    RUVIA_CHECK(contentTypeMatches("application/json ; charset=utf-8", "application/json"));
    // The media type is matched case-insensitively.
    RUVIA_CHECK(contentTypeMatches("APPLICATION/JSON", "application/json"));
    RUVIA_CHECK(contentTypeMatches("application/x-www-form-urlencoded",
                                   "application/x-www-form-urlencoded"));
}

RUVIA_TEST(content_type_matches_rejects_mismatches) {
    RUVIA_CHECK(!contentTypeMatches("text/html", "application/json"));
    RUVIA_CHECK(!contentTypeMatches("", "application/json"));            // empty content type
    RUVIA_CHECK(!contentTypeMatches("application/jsonx", "application/json"));  // exact, not prefix
    RUVIA_CHECK(!contentTypeMatches("application/json", "application/jsonx"));
}
