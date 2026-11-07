#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_field_values.h"

// A quoted-string in a field value is opaque: a delimiter inside it never splits the field.

TEST(Ruvia, public_quoted_field_visitors_preserve_quoted_delimiters) {
    std::string_view parameter_name;
    std::string_view parameter_value;
    ruvia::http_visit_semicolon_parameters_quoted_field(
        R"(for="[2001:db8::1];zone"; proto=https)",
        [&](std::string_view name, std::string_view value) {
            parameter_name = name;
            parameter_value = ruvia::http_trim_quoted_field_value(value);
            return true;
        });
    EXPECT_EQ(parameter_name, std::string_view("proto"));
    EXPECT_EQ(parameter_value, std::string_view("https"));

    std::size_t item_count = 0;
    ruvia::http_visit_comma_separated_quoted_field_items(
        R"(for="192.0.2.1,192.0.2.2", proto=https)",
        [&](std::string_view) {
            ++item_count;
            return true;
        });
    EXPECT_EQ(item_count, std::size_t{2});
}

TEST(Ruvia, public_quoted_string_cursor_validates_and_decodes) {
    std::string decoded;
    ruvia::http_quoted_string_cursor cursor(R"("a\"b\\c";rest)");
    unsigned char byte = 0;
    while (cursor.next(byte)) {
        decoded.push_back(static_cast<char>(byte));
    }
    EXPECT_TRUE(cursor.closed());
    EXPECT_EQ(decoded, std::string_view(R"(a"b\c)"));
    EXPECT_EQ(cursor.consumed(), std::size_t{9});

    EXPECT_EQ(ruvia::http_quoted_string_size(R"("x" tail)"), std::size_t{3});
    EXPECT_TRUE(ruvia::http_is_quoted_string(R"("")"));
    EXPECT_TRUE(ruvia::http_is_quoted_string("\"\t obs\x80\""));
    EXPECT_TRUE(!ruvia::http_is_quoted_string(""));
    EXPECT_TRUE(!ruvia::http_is_quoted_string("token"));
    EXPECT_TRUE(!ruvia::http_is_quoted_string(R"("open)"));
    EXPECT_TRUE(!ruvia::http_is_quoted_string(R"("a"b")"));
    EXPECT_TRUE(!ruvia::http_is_quoted_string(R"("trailing\")"));
    EXPECT_TRUE(!ruvia::http_is_quoted_string("\"ctl\x01\""));
    EXPECT_TRUE(!ruvia::http_is_quoted_string("\"del\x7f\""));

    EXPECT_EQ(ruvia::http_find_unquoted_delimiter(R"(a="x,\",y",b)", 0, ','), std::size_t{10});
}

TEST(Ruvia, public_unquoted_delimiter_preserves_quote_state_before_start) {
    constexpr std::string_view field = R"(p="a;b";q=x)";
    for (std::size_t start = 0; start <= 7; ++start) {
        EXPECT_EQ(ruvia::http_find_unquoted_delimiter(field, start, ';'), std::size_t{7});
    }
    EXPECT_EQ(ruvia::http_find_unquoted_delimiter(field, 8, ';'), field.size());
    EXPECT_EQ(ruvia::http_find_unquoted_delimiter(field, field.size() + 1, ';'), field.size());

    constexpr std::string_view escaped = R"(p="a\";b";q=x)";
    for (std::size_t start = 4; start <= 9; ++start) {
        EXPECT_EQ(ruvia::http_find_unquoted_delimiter(escaped, start, ';'), std::size_t{9});
    }
    EXPECT_EQ(ruvia::http_find_unquoted_delimiter(R"(p="a;b)", 4, ';'), std::size_t{6});
}

TEST(Ruvia, accept_encoding_quality_unquoted_unchanged) {
    using ruvia::http_accepts_encoding;
    EXPECT_TRUE(http_accepts_encoding("", "identity"));
    EXPECT_TRUE(!http_accepts_encoding("", "gzip"));
    EXPECT_TRUE(http_accepts_encoding("gzip;q=0.5, br", "br"));
    EXPECT_TRUE(http_accepts_encoding("gzip;q=0.5, br", "gzip"));
    EXPECT_TRUE(!http_accepts_encoding("gzip;q=0", "gzip"));
}

TEST(Ruvia, accept_quality_quoted_comma_does_not_split_item) {
    using ruvia::http_accepts_encoding;
    EXPECT_TRUE(!http_accepts_encoding(R"(gzip;note="a,b";q=0)", "gzip"));
}

static_assert(std::is_constructible_v<ruvia::http_quoted_string_cursor, const std::string&>);
static_assert(std::is_constructible_v<ruvia::http_quoted_string_cursor, std::string_view>);
static_assert(!std::is_constructible_v<ruvia::http_quoted_string_cursor, std::string>);
static_assert(!std::is_constructible_v<ruvia::http_quoted_string_cursor, std::pmr::string>);
