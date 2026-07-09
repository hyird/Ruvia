#include "test_harness.h"

#include <string_view>

#include "HeaderTokenUtils.h"

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

RUVIA_TEST(header_has_token_skips_empty_list_items) {
    // Doubled, leading, and trailing commas (which real proxies emit) produce
    // empty list items that must be skipped -- not matched, and not stopping the
    // scan from reaching the real tokens.
    RUVIA_CHECK(httpHasToken("gzip,,deflate", "deflate"));
    RUVIA_CHECK(httpHasToken(",gzip", "gzip"));
    RUVIA_CHECK(httpHasToken("gzip,", "gzip"));
    RUVIA_CHECK(httpHasToken(" , gzip , ", "gzip"));
    // A list of only empty items never matches anything.
    RUVIA_CHECK(!httpHasToken(",,", "gzip"));
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

RUVIA_TEST(header_decode_quoted_pairs) {
    // RFC 7230 §3.2.6: inside a quoted-string, "\X" represents the octet X. The
    // input is already quote-trimmed; a valid unquoted token has no backslash, so
    // every '\' is an escape (a trailing lone '\' from malformed input is kept).
    auto* resource = std::pmr::get_default_resource();
    const auto decode = [resource](std::string_view value) {
        std::pmr::string out(resource);
        ruvia::detail::httpAppendDecodedQuotedPairs(out, value);
        return std::string(out.data(), out.size());
    };
    RUVIA_CHECK_EQ(decode("plain"), std::string("plain"));      // no escapes -> unchanged
    RUVIA_CHECK_EQ(decode("a\\\"b"), std::string("a\"b"));      // \" -> "
    RUVIA_CHECK_EQ(decode("x\\\\y"), std::string("x\\y"));      // two backslashes -> one
    RUVIA_CHECK_EQ(decode("\\a\\b\\c"), std::string("abc"));    // each pair unescaped
    RUVIA_CHECK_EQ(decode("end\\"), std::string("end\\"));      // trailing lone '\' kept verbatim
}
