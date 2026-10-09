#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/util/http_ows.h"
#include "ruvia/http/http_field_whitespace.h"

#include "coding/http_transfer_encoding.h"
#include "field/http_entity_tag.h"
#include "field/http_media_type.h"
#include "field/http_quality_value.h"
#include "http2/http2_frame_payload.h"
#include "parser/mime_field_grammar.h"
#include "test_harness.h"
#include "websocket/http_websocket_handshake_fields.h"

namespace {

using ruvia::detail::http_has_exact_token;
using ruvia::detail::http_has_token;
using ruvia::detail::http_trim_quotes;

struct match_any_header_token final {
    [[nodiscard]] constexpr bool operator()(std::string_view) const noexcept {
        return true;
    }
};

}  // namespace

RUVIA_TEST(http_field_ows_trims_only_space_and_tab) {
    RUVIA_CHECK_EQ(ruvia::http_trim_ows(" \t text/plain \t"), std::string_view("text/plain"));
    RUVIA_CHECK_EQ(ruvia::http_trim_ows("\ntext/plain\n"), std::string_view("\ntext/plain\n"));
}

RUVIA_TEST(header_has_token_case_insensitive) {
    RUVIA_CHECK(http_has_token("gzip, deflate", "deflate"));
    RUVIA_CHECK(http_has_token("gzip, deflate", "GZIP"));  // case-insensitive
    RUVIA_CHECK(http_has_token("keep-alive, Upgrade", "upgrade"));
    RUVIA_CHECK(http_has_token("  gzip  ,  deflate ", "deflate"));  // surrounding OWS tolerated
    RUVIA_CHECK(http_has_token("gzip", "gzip"));                    // a single token
    RUVIA_CHECK(!http_has_token("gzip, deflate", "br"));
    RUVIA_CHECK(!http_has_token("gzipx", "gzip"));  // a substring is not a token
    RUVIA_CHECK(!http_has_token("gzip", ""));       // empty expected
    RUVIA_CHECK(!http_has_token("", "gzip"));       // empty value
}

RUVIA_TEST(header_has_token_skips_empty_list_items) {
    // Doubled, leading, and trailing commas (which real proxies emit) produce
    // empty list items that must be skipped -- not matched, and not stopping the
    // scan from reaching the real tokens.
    RUVIA_CHECK(http_has_token("gzip,,deflate", "deflate"));
    RUVIA_CHECK(http_has_token(",gzip", "gzip"));
    RUVIA_CHECK(http_has_token("gzip,", "gzip"));
    RUVIA_CHECK(http_has_token(" , gzip , ", "gzip"));
    // A list of only empty items never matches anything.
    RUVIA_CHECK(!http_has_token(",,", "gzip"));
}

RUVIA_TEST(header_has_exact_token_case_sensitive) {
    RUVIA_CHECK(http_has_exact_token("gzip, deflate", "deflate"));
    RUVIA_CHECK(!http_has_exact_token("gzip, DEFLATE", "deflate"));  // case-sensitive
    RUVIA_CHECK(http_has_exact_token("a, b, c", "b"));
    RUVIA_CHECK(!http_has_exact_token("a, b, c", "d"));
}

RUVIA_TEST(header_trim_quotes) {
    RUVIA_CHECK_EQ(http_trim_quotes("\"abc\""), std::string_view("abc"));
    RUVIA_CHECK_EQ(http_trim_quotes("abc"), std::string_view("abc"));      // no quotes
    RUVIA_CHECK_EQ(http_trim_quotes("\"\""), std::string_view(""));        // empty quoted
    RUVIA_CHECK_EQ(http_trim_quotes("\""), std::string_view("\""));        // one quote is too short
    RUVIA_CHECK_EQ(http_trim_quotes("\"abc"), std::string_view("\"abc"));  // only a leading quote
}

RUVIA_TEST(header_decode_quoted_pairs) {
    // RFC 7230 §3.2.6: inside a quoted-string, "\X" represents the octet X. The
    // input is already quote-trimmed; a valid unquoted token has no backslash, so
    // every '\' is an escape (a trailing lone '\' from malformed input is kept).
    auto* resource = std::pmr::get_default_resource();
    const auto decode = [resource](std::string_view value) {
        std::pmr::string out(resource);
        ruvia::detail::http_append_decoded_quoted_pairs(out, value);
        return std::string(out.data(), out.size());
    };
    RUVIA_CHECK_EQ(decode("plain"), std::string("plain"));    // no escapes -> unchanged
    RUVIA_CHECK_EQ(decode("a\\\"b"), std::string("a\"b"));    // \" -> "
    RUVIA_CHECK_EQ(decode("x\\\\y"), std::string("x\\y"));    // two backslashes -> one
    RUVIA_CHECK_EQ(decode("\\a\\b\\c"), std::string("abc"));  // each pair unescaped
    RUVIA_CHECK_EQ(decode("end\\"), std::string("end\\"));    // trailing lone '\' kept verbatim
}
