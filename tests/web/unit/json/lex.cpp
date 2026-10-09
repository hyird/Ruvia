#include <string_view>

#include "ruvia/web/detail/json/json_lex.h"

#include "test_harness.h"

namespace {

using ruvia::detail::consume_json_char;
using ruvia::detail::consume_json_literal;
using ruvia::detail::skip_json_whitespace;

}  // namespace

RUVIA_TEST(json_lex_skip_whitespace) {
    // The four JSON whitespace bytes (RFC 8259): space, tab, CR, LF.
    std::string_view in = "  \t\r\n abc";
    skip_json_whitespace(in);
    RUVIA_CHECK_EQ(in, std::string_view("abc"));

    std::string_view none = "xyz";
    skip_json_whitespace(none);
    RUVIA_CHECK_EQ(none, std::string_view("xyz"));

    std::string_view all_ws = "  \t\n";
    skip_json_whitespace(all_ws);
    RUVIA_CHECK(all_ws.empty());
}

RUVIA_TEST(json_lex_consume_char) {
    std::string_view in = "  { rest";
    RUVIA_CHECK(consume_json_char(in, '{'));
    RUVIA_CHECK_EQ(in, std::string_view(" rest"));  // leading whitespace skipped, '{' consumed

    // A mismatch skips whitespace but does not consume the character.
    std::string_view wrong = "  x";
    RUVIA_CHECK(!consume_json_char(wrong, '{'));
    RUVIA_CHECK_EQ(wrong, std::string_view("x"));

    // Whitespace-only input has nothing to consume.
    std::string_view empty = "   ";
    RUVIA_CHECK(!consume_json_char(empty, '}'));
}

RUVIA_TEST(json_lex_consume_literal) {
    std::string_view in = "  true, ";
    RUVIA_CHECK(consume_json_literal(in, "true"));
    RUVIA_CHECK_EQ(in, std::string_view(", "));

    // A shorter input that is only a prefix of the literal does not match.
    std::string_view partial = "tru";
    RUVIA_CHECK(!consume_json_literal(partial, "true"));
    RUVIA_CHECK_EQ(partial, std::string_view("tru"));

    // The literal is consumed even when more content follows.
    std::string_view longer = "nullish";
    RUVIA_CHECK(consume_json_literal(longer, "null"));
    RUVIA_CHECK_EQ(longer, std::string_view("ish"));
}
