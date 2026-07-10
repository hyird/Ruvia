#include "test_harness.h"

#include <string_view>

#include "ruvia/web/detail/json/JsonLex.h"

namespace {

using ruvia::detail::consumeJsonChar;
using ruvia::detail::consumeJsonLiteral;
using ruvia::detail::skipJsonWhitespace;

}  // namespace

RUVIA_TEST(json_lex_skip_whitespace) {
    // The four JSON whitespace bytes (RFC 8259): space, tab, CR, LF.
    std::string_view in = "  \t\r\n abc";
    skipJsonWhitespace(in);
    RUVIA_CHECK_EQ(in, std::string_view("abc"));

    std::string_view none = "xyz";
    skipJsonWhitespace(none);
    RUVIA_CHECK_EQ(none, std::string_view("xyz"));

    std::string_view allWs = "  \t\n";
    skipJsonWhitespace(allWs);
    RUVIA_CHECK(allWs.empty());
}

RUVIA_TEST(json_lex_consume_char) {
    std::string_view in = "  { rest";
    RUVIA_CHECK(consumeJsonChar(in, '{'));
    RUVIA_CHECK_EQ(in, std::string_view(" rest"));  // leading whitespace skipped, '{' consumed

    // A mismatch skips whitespace but does not consume the character.
    std::string_view wrong = "  x";
    RUVIA_CHECK(!consumeJsonChar(wrong, '{'));
    RUVIA_CHECK_EQ(wrong, std::string_view("x"));

    // Whitespace-only input has nothing to consume.
    std::string_view empty = "   ";
    RUVIA_CHECK(!consumeJsonChar(empty, '}'));
}

RUVIA_TEST(json_lex_consume_literal) {
    std::string_view in = "  true, ";
    RUVIA_CHECK(consumeJsonLiteral(in, "true"));
    RUVIA_CHECK_EQ(in, std::string_view(", "));

    // A shorter input that is only a prefix of the literal does not match.
    std::string_view partial = "tru";
    RUVIA_CHECK(!consumeJsonLiteral(partial, "true"));
    RUVIA_CHECK_EQ(partial, std::string_view("tru"));

    // The literal is consumed even when more content follows.
    std::string_view longer = "nullish";
    RUVIA_CHECK(consumeJsonLiteral(longer, "null"));
    RUVIA_CHECK_EQ(longer, std::string_view("ish"));
}
