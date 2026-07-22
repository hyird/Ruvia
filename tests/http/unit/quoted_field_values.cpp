#include "field_parsing_fixture.h"

// A quoted-string in a field value is opaque: a delimiter inside it never splits the field.

RUVIA_TEST(semicolon_params_quoted_semicolon_in_value) {
    using ruvia::detail::httpFindSemicolonParameterQuoted;
    // A ';' inside a quoted value must not split the parameter.
    const std::string_view v = R"(form-data; name="a;b"; filename="c;d.txt")";
    RUVIA_CHECK_EQ(
        httpFindSemicolonParameterQuoted(v, "name").value_or("?"),
        std::string_view(R"("a;b")"));
    RUVIA_CHECK_EQ(
        httpFindSemicolonParameterQuoted(v, "filename").value_or("?"),
        std::string_view(R"("c;d.txt")"));
}

RUVIA_TEST(semicolon_params_quoted_matches_plain_when_unquoted) {
    using ruvia::detail::httpFindSemicolonParameter;
    using ruvia::detail::httpFindSemicolonParameterQuoted;
    const std::string_view v = "form-data; name=foo; filename=bar.txt";
    RUVIA_CHECK_EQ(
        httpFindSemicolonParameterQuoted(v, "name").value_or("?"),
        httpFindSemicolonParameter(v, "name").value_or("!"));
    RUVIA_CHECK_EQ(
        httpFindSemicolonParameterQuoted(v, "filename").value_or("?"),
        std::string_view("bar.txt"));
}

RUVIA_TEST(semicolon_params_quoted_uses_last_match) {
    using ruvia::detail::httpFindSemicolonParameterQuoted;
    const std::string_view v = R"(form-data; name="first"; filename=a.txt; name="second")";
    RUVIA_CHECK_EQ(
        httpFindSemicolonParameterQuoted(v, "name").value_or("?"),
        std::string_view(R"("second")"));
}

RUVIA_TEST(accept_quality_quoted_semicolon_param) {
    using ruvia::detail::httpAcceptsMediaType;
    // A ';' inside a quoted media-range parameter must NOT be read as a parameter
    // separator when locating q (RFC 7231 §5.3.2). Before unifying onto the quote-aware
    // scanner this mis-read "q=0" from inside the quotes and rejected the type.
    RUVIA_CHECK(httpAcceptsMediaType(
        R"(application/json;version="a;q=0";q=0.9)",
        R"(application/json;version="a;q=0")"));
    // Regressions: a real q=0 still means "not accepted", and a normal q is honored.
    RUVIA_CHECK(!httpAcceptsMediaType("application/json;q=0", "application/json"));
    RUVIA_CHECK(httpAcceptsMediaType("text/html;q=0.8", "text/html"));
}

RUVIA_TEST(accept_encoding_quality_unquoted_unchanged) {
    using ruvia::detail::httpAcceptsEncoding;
    RUVIA_CHECK(httpAcceptsEncoding("gzip;q=0.5, br", "br"));
    RUVIA_CHECK(httpAcceptsEncoding("gzip;q=0.5, br", "gzip"));
    RUVIA_CHECK(!httpAcceptsEncoding("gzip;q=0", "gzip"));
}

RUVIA_TEST(accept_quality_quoted_comma_does_not_split_item) {
    using ruvia::detail::httpAcceptsEncoding;
    using ruvia::detail::httpAcceptsMediaType;

    RUVIA_CHECK(!httpAcceptsMediaType(
        R"(application/json;version="a,b";q=0)", "application/json"));
    RUVIA_CHECK(!httpAcceptsEncoding(R"(gzip;note="a,b";q=0)", "gzip"));
}
