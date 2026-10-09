#include <string_view>

#include "field/http_entity_tag.h"
#include "test_harness.h"

// Small field-value primitives: the weak-etag prefix.

RUVIA_TEST(http_trim_weak_etag_prefix) {
    using ruvia::detail::http_trim_weak_etag_prefix;
    RUVIA_CHECK_EQ(http_trim_weak_etag_prefix("W/\"abc\""), std::string_view("\"abc\""));
    RUVIA_CHECK_EQ(
        http_trim_weak_etag_prefix("\"abc\""), std::string_view("\"abc\""));  // strong etag unchanged
    RUVIA_CHECK_EQ(http_trim_weak_etag_prefix("W/"), std::string_view(""));
    RUVIA_CHECK_EQ(http_trim_weak_etag_prefix("W"), std::string_view("W"));  // needs both prefix chars
}

RUVIA_TEST(http_strong_etag_is_quoted_opaque_tag) {
    using ruvia::detail::http_is_strong_etag;
    RUVIA_CHECK(http_is_strong_etag(R"("abc")"));
    RUVIA_CHECK(!http_is_strong_etag(R"(W/"abc")"));
    RUVIA_CHECK(!http_is_strong_etag("abc"));
    RUVIA_CHECK(!http_is_strong_etag(""));
}
