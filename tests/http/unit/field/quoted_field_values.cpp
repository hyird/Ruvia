#include "ruvia/http/http_field_values.h"

#include "field_parsing_fixture.h"

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

RUVIA_TEST(semicolon_params_quoted_semicolon_in_value) {
    using ruvia::detail::http_find_semicolon_parameter_quoted;
    // A ';' inside a quoted value must not split the parameter.
    const std::string_view v = R"(form-data; name="a;b"; filename="c;d.txt")";
    RUVIA_CHECK_EQ(
        http_find_semicolon_parameter_quoted(v, "name").value_or("?"), std::string_view(R"("a;b")"));
    RUVIA_CHECK_EQ(http_find_semicolon_parameter_quoted(v, "filename").value_or("?"),
        std::string_view(R"("c;d.txt")"));
}

RUVIA_TEST(semicolon_params_quoted_matches_plain_when_unquoted) {
    using ruvia::detail::http_find_semicolon_parameter;
    using ruvia::detail::http_find_semicolon_parameter_quoted;
    const std::string_view v = "form-data; name=foo; filename=bar.txt";
    RUVIA_CHECK_EQ(http_find_semicolon_parameter_quoted(v, "name").value_or("?"),
        http_find_semicolon_parameter(v, "name").value_or("!"));
    RUVIA_CHECK_EQ(
        http_find_semicolon_parameter_quoted(v, "filename").value_or("?"), std::string_view("bar.txt"));
}

RUVIA_TEST(semicolon_params_quoted_uses_last_match) {
    using ruvia::detail::http_find_semicolon_parameter_quoted;
    const std::string_view v = R"(form-data; name="first"; filename=a.txt; name="second")";
    RUVIA_CHECK_EQ(
        http_find_semicolon_parameter_quoted(v, "name").value_or("?"), std::string_view(R"("second")"));
}

RUVIA_TEST(accept_quality_quoted_semicolon_param) {
    using ruvia::detail::http_accepts_media_type;
    // A ';' inside a quoted media-range parameter must NOT be read as a parameter
    // separator when locating q (RFC 7231 §5.3.2). Before unifying onto the quote-aware
    // scanner this mis-read "q=0" from inside the quotes and rejected the type.
    RUVIA_CHECK(http_accepts_media_type(
        R"(application/json;version="a;q=0";q=0.9)", R"(application/json;version="a;q=0")"));
    // Regressions: a real q=0 still means "not accepted", and a normal q is honored.
    RUVIA_CHECK(!http_accepts_media_type("application/json;q=0", "application/json"));
    RUVIA_CHECK(http_accepts_media_type("text/html;q=0.8", "text/html"));
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
    using ruvia::detail::http_accepts_media_type;

    RUVIA_CHECK(!http_accepts_media_type(R"(application/json;version="a,b";q=0)", "application/json"));
    RUVIA_CHECK(!http_accepts_encoding(R"(gzip;note="a,b";q=0)", "gzip"));
}
