#include "field_parsing_fixture.h"

// Quoted names and boundaries in a multipart body, and the delimiter terminator each form requires.

RUVIA_TEST(multipart_part_headers_quoted_name_with_semicolon) {
    const std::string_view block =
        "Content-Disposition: form-data; name=\"a;b\"; filename=\"up;load.txt\"\r\n"
        "Content-Type: text/plain";
    const auto result = ruvia::detail::httpParseMultipartPartHeaders(block);
    const HttpMultipartPartHeaders* headers = result.headers();
    RUVIA_CHECK(headers != nullptr);
    if (headers != nullptr) {
        RUVIA_CHECK_EQ(headers->name(), std::string_view("a;b"));
        RUVIA_CHECK_EQ(headers->filename(), std::string_view("up;load.txt"));
        RUVIA_CHECK_EQ(headers->contentType(), std::string_view("text/plain"));
    }
}

RUVIA_TEST(multipart_boundary_quoted_with_mime_special) {
    const std::string_view contentType = R"(multipart/form-data; boundary="a:b")";
    const auto result = ruvia::parseMultipartBoundary(contentType);
    RUVIA_CHECK(result.boundary() != nullptr);
    RUVIA_CHECK_EQ(result.boundary()->value(), std::string_view("a:b"));
}

RUVIA_TEST(multipart_boundary_prefix_requires_delimiter_terminator) {
    using ruvia::detail::httpFindMultipartBodyDelimiter;
    // "abc" appears as a substring of "abcXYZ" in the body; that is NOT a delimiter (a delimiter
    // must be followed by CRLF or "--"). The scan must skip the false match and find the real one.
    const std::string_view body = "data\r\n--abcXYZ tail\r\n--abc\r\n";
    const auto match = httpFindMultipartBodyDelimiter(body, ruvia::MultipartBoundary("abc"), true);
    const auto* part = match.part();
    RUVIA_CHECK(part != nullptr);
    if (part != nullptr) {
        RUVIA_CHECK_EQ(part->offset(), body.find("\r\n--abc\r\n"));
    }
}

RUVIA_TEST(multipart_boundary_line_requires_delimiter_terminator) {
    using ruvia::detail::httpFindInitialMultipartDelimiter;
    // Same for the opening delimiter. The real candidate begins a new line;
    // the matching bytes embedded in preamble text are not eligible.
    const std::string_view body = "--abcXYZ junk--abc\r\n\r\n--abc\r\nrest";
    const auto match = httpFindInitialMultipartDelimiter(body, ruvia::MultipartBoundary("abc"), true);
    const auto* part = match.part();
    RUVIA_CHECK(part != nullptr);
    if (part != nullptr) {
        RUVIA_CHECK_EQ(part->offset(), body.rfind("--abc\r\n"));
    }
    // A close delimiter ("--abc--") is a valid terminator too.
    const std::string_view closing = "--abc--\r\n";
    const auto closeMatch = httpFindInitialMultipartDelimiter(closing, ruvia::MultipartBoundary("abc"), true);
    RUVIA_CHECK(closeMatch.close() != nullptr);
}
