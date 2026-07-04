#include "test_harness.h"

#include <string_view>

#include "http/HeaderTokenUtils.h"

namespace {

using ruvia::detail::httpHasExactToken;
using ruvia::detail::httpHasToken;
using ruvia::detail::httpTrimQuotes;

}  // namespace

RUVIA_TEST(header_has_token_case_insensitive) {
    RUVIA_CHECK(httpHasToken("gzip, deflate", "deflate"));
    RUVIA_CHECK(httpHasToken("gzip, deflate", "GZIP"));            // case-insensitive
    RUVIA_CHECK(httpHasToken("keep-alive, Upgrade", "upgrade"));
    RUVIA_CHECK(httpHasToken("  gzip  ,  deflate ", "deflate"));   // surrounding OWS tolerated
    RUVIA_CHECK(httpHasToken("gzip", "gzip"));                     // a single token
    RUVIA_CHECK(!httpHasToken("gzip, deflate", "br"));
    RUVIA_CHECK(!httpHasToken("gzipx", "gzip"));                   // a substring is not a token
    RUVIA_CHECK(!httpHasToken("gzip", ""));                        // empty expected
    RUVIA_CHECK(!httpHasToken("", "gzip"));                        // empty value
}

RUVIA_TEST(header_has_exact_token_case_sensitive) {
    RUVIA_CHECK(httpHasExactToken("gzip, deflate", "deflate"));
    RUVIA_CHECK(!httpHasExactToken("gzip, DEFLATE", "deflate"));   // case-sensitive
    RUVIA_CHECK(httpHasExactToken("a, b, c", "b"));
    RUVIA_CHECK(!httpHasExactToken("a, b, c", "d"));
}

RUVIA_TEST(header_trim_quotes) {
    RUVIA_CHECK_EQ(httpTrimQuotes("\"abc\""), std::string_view("abc"));
    RUVIA_CHECK_EQ(httpTrimQuotes("abc"), std::string_view("abc"));      // no quotes
    RUVIA_CHECK_EQ(httpTrimQuotes("\"\""), std::string_view(""));         // empty quoted
    RUVIA_CHECK_EQ(httpTrimQuotes("\""), std::string_view("\""));         // one quote is too short
    RUVIA_CHECK_EQ(httpTrimQuotes("\"abc"), std::string_view("\"abc"));   // only a leading quote
}
