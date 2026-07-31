#include "test_harness.h"

#include <string_view>

#include "ruvia/http/detail/field/HttpEntityTag.h"

// Small field-value primitives: the weak-etag prefix.

RUVIA_TEST(http_trim_weak_etag_prefix) {
    using ruvia::detail::httpTrimWeakEtagPrefix;
    RUVIA_CHECK_EQ(httpTrimWeakEtagPrefix("W/\"abc\""), std::string_view("\"abc\""));
    RUVIA_CHECK_EQ(httpTrimWeakEtagPrefix("\"abc\""), std::string_view("\"abc\""));  // strong etag unchanged
    RUVIA_CHECK_EQ(httpTrimWeakEtagPrefix("W/"), std::string_view(""));
    RUVIA_CHECK_EQ(httpTrimWeakEtagPrefix("W"), std::string_view("W"));  // needs both prefix chars
}
