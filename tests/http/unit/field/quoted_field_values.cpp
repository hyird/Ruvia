#include <cstddef>
#include <string>
#include <string_view>

#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_field_values.h"

#include "test_harness.h"

// A quoted-string in a field value is opaque: a delimiter inside it never splits the field.

RUVIA_TEST(public_quoted_field_visitors_preserve_quoted_delimiters) {
    std::string_view parameter_name;
    std::string_view parameter_value;
    ruvia::http_visit_semicolon_parameters_quoted_field(
        R"(for="[2001:db8::1];zone"; proto=https)",
        [&](std::string_view name, std::string_view value) {
            parameter_name = name;
            parameter_value = ruvia::http_trim_quoted_field_value(value);
            return true;
        });
    RUVIA_CHECK_EQ(parameter_name, std::string_view("proto"));
    RUVIA_CHECK_EQ(parameter_value, std::string_view("https"));

    std::size_t item_count = 0;
    ruvia::http_visit_comma_separated_quoted_field_items(
        R"(for="192.0.2.1,192.0.2.2", proto=https)",
        [&](std::string_view) {
            ++item_count;
            return true;
        });
    RUVIA_CHECK_EQ(item_count, std::size_t{2});
}

RUVIA_TEST(public_quoted_string_cursor_validates_and_decodes) {
    std::string decoded;
    ruvia::http_quoted_string_cursor cursor(R"("a\"b\\c";rest)");
    unsigned char byte = 0;
    while (cursor.next(byte)) {
        decoded.push_back(static_cast<char>(byte));
    }
    RUVIA_CHECK(cursor.closed());
    RUVIA_CHECK_EQ(decoded, std::string_view(R"(a"b\c)"));
    RUVIA_CHECK_EQ(cursor.consumed(), std::size_t{9});

    RUVIA_CHECK_EQ(ruvia::http_quoted_string_size(R"("x" tail)"), std::size_t{3});
    RUVIA_CHECK(ruvia::http_is_quoted_string(R"("")"));
    RUVIA_CHECK(ruvia::http_is_quoted_string("\"\t obs\x80\""));
    RUVIA_CHECK(!ruvia::http_is_quoted_string(""));
    RUVIA_CHECK(!ruvia::http_is_quoted_string("token"));
    RUVIA_CHECK(!ruvia::http_is_quoted_string(R"("open)"));
    RUVIA_CHECK(!ruvia::http_is_quoted_string(R"("a"b")"));
    RUVIA_CHECK(!ruvia::http_is_quoted_string(R"("trailing\")"));
    RUVIA_CHECK(!ruvia::http_is_quoted_string("\"ctl\x01\""));
    RUVIA_CHECK(!ruvia::http_is_quoted_string("\"del\x7f\""));

    RUVIA_CHECK_EQ(ruvia::http_find_unquoted_delimiter(R"(a="x,\",y",b)", 0, ','), std::size_t{10});
}

RUVIA_TEST(accept_encoding_quality_unquoted_unchanged) {
    using ruvia::http_accepts_encoding;
    RUVIA_CHECK(http_accepts_encoding("", "identity"));
    RUVIA_CHECK(!http_accepts_encoding("", "gzip"));
    RUVIA_CHECK(http_accepts_encoding("gzip;q=0.5, br", "br"));
    RUVIA_CHECK(http_accepts_encoding("gzip;q=0.5, br", "gzip"));
    RUVIA_CHECK(!http_accepts_encoding("gzip;q=0", "gzip"));
}

RUVIA_TEST(accept_quality_quoted_comma_does_not_split_item) {
    using ruvia::http_accepts_encoding;
    RUVIA_CHECK(!http_accepts_encoding(R"(gzip;note="a,b";q=0)", "gzip"));
}
