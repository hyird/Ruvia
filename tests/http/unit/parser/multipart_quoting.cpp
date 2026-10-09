#include "field_parsing_fixture.h"

// Quoted names and boundaries in a multipart body, and the delimiter terminator each form requires.

RUVIA_TEST(multipart_part_headers_quoted_name_with_semicolon) {
    const std::string_view block =
        "Content-Disposition: form-data; name=\"a;b\"; filename=\"up;load.txt\"\r\n"
        "Content-Type: text/plain";
    const auto result_value = ruvia::detail::http_parse_multipart_part_headers(block);
    const http_multipart_part_headers* headers = result_value.headers();
    RUVIA_CHECK(headers != nullptr);
    if (headers != nullptr) {
        RUVIA_CHECK_EQ(headers->name(), std::string_view("a;b"));
        RUVIA_CHECK_EQ(headers->filename(), std::string_view("up;load.txt"));
        RUVIA_CHECK_EQ(headers->content_type(), std::string_view("text/plain"));
    }
}

RUVIA_TEST(multipart_boundary_quoted_with_mime_special) {
    const std::string_view content_type_value = R"(multipart/form-data; boundary="a:b")";
    const auto result_value = ruvia::parse_multipart_boundary(content_type_value);
    RUVIA_CHECK(result_value.boundary() != nullptr);
    RUVIA_CHECK_EQ(result_value.boundary()->value(), std::string_view("a:b"));
}

RUVIA_TEST(multipart_boundary_prefix_requires_delimiter_terminator) {
    using ruvia::detail::http_find_multipart_body_delimiter;
    // "abc" appears as a substring of "abcXYZ" in the body; that is NOT a delimiter (a delimiter
    // must be followed by CRLF or "--"). The scan must skip the false match and find the real one.
    const std::string_view body = "data\r\n--abcXYZ tail\r\n--abc\r\n";
    const auto match = http_find_multipart_body_delimiter(body, ruvia::multipart_boundary("abc"), true);
    const auto* part = match.part();
    RUVIA_CHECK(part != nullptr);
    if (part != nullptr) {
        RUVIA_CHECK_EQ(part->offset(), body.find("\r\n--abc\r\n"));
    }
}

RUVIA_TEST(multipart_boundary_line_requires_delimiter_terminator) {
    using ruvia::detail::http_find_initial_multipart_delimiter;
    // Same for the opening delimiter. The real candidate begins a new line;
    // the matching bytes embedded in preamble text are not eligible.
    const std::string_view body = "--abcXYZ junk--abc\r\n\r\n--abc\r\nrest";
    const auto match =
        http_find_initial_multipart_delimiter(body, ruvia::multipart_boundary("abc"), true);
    const auto* part = match.part();
    RUVIA_CHECK(part != nullptr);
    if (part != nullptr) {
        RUVIA_CHECK_EQ(part->offset(), body.rfind("--abc\r\n"));
    }
    // A close delimiter ("--abc--") is a valid terminator too.
    const std::string_view closing = "--abc--\r\n";
    const auto close_match =
        http_find_initial_multipart_delimiter(closing, ruvia::multipart_boundary("abc"), true);
    RUVIA_CHECK(close_match.close() != nullptr);
}
