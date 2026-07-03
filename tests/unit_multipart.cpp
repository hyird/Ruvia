#include "test_harness.h"

#include <cstddef>
#include <string_view>

#include "http/MultipartParsing.h"

// A boundary delimiter ends with CRLF (next part) or "--" (close). A lone '-'
// after the boundary token is NOT a delimiter: "--<boundary>-x" must be skipped,
// not mistaken for a close delimiter. Regression for the terminator check in
// httpMultipartBoundaryAt (RFC 2046 §5.1.1).
RUVIA_TEST(multipart_boundary_lone_dash_is_not_a_delimiter) {
    using ruvia::detail::httpFindMultipartBoundaryPrefix;
    // The first "\r\n--abc-x" is a false delimiter ('-' then 'x', neither CRLF nor
    // "--"); the real one is the later "\r\n--abc\r\n". Before the fix this returned 0.
    const std::string_view body = "\r\n--abc-x\r\n--abc\r\n";
    RUVIA_CHECK_EQ(
        httpFindMultipartBoundaryPrefix(body, "abc"),
        body.find("\r\n--abc\r\n"));
    RUVIA_CHECK(httpFindMultipartBoundaryPrefix(body, "abc") != std::size_t{0});
}

RUVIA_TEST(multipart_boundary_close_delimiter_still_matches) {
    using ruvia::detail::httpFindMultipartBoundaryLine;
    using ruvia::detail::httpFindMultipartBoundaryPrefix;
    // "--" after the boundary (close-delimiter) is a valid terminator.
    RUVIA_CHECK_EQ(httpFindMultipartBoundaryLine("--abc--\r\n", "abc"), std::size_t{0});
    RUVIA_CHECK_EQ(
        httpFindMultipartBoundaryPrefix("body\r\n--abc--\r\n", "abc"),
        std::string_view("body\r\n--abc--\r\n").find("\r\n--abc--"));
    // CRLF after the boundary (next part) is still a valid terminator.
    RUVIA_CHECK_EQ(httpFindMultipartBoundaryLine("--abc\r\nrest", "abc"), std::size_t{0});
}

// Boundary extraction from the Content-Type header (RFC 7578).
RUVIA_TEST(multipart_boundary_from_content_type) {
    using ruvia::detail::httpParseMultipartBoundary;
    using ruvia::detail::HttpMultipartBoundaryStatus;
    std::string_view boundary;
    RUVIA_CHECK(httpParseMultipartBoundary("multipart/form-data; boundary=abc123", boundary) ==
                HttpMultipartBoundaryStatus::kOk);
    RUVIA_CHECK_EQ(boundary, std::string_view("abc123"));
    RUVIA_CHECK(httpParseMultipartBoundary(R"(multipart/form-data; boundary="a b")", boundary) ==
                HttpMultipartBoundaryStatus::kOk);
    RUVIA_CHECK_EQ(boundary, std::string_view("a b"));  // quotes stripped
    // Wrong media type and a missing boundary parameter are distinct errors.
    RUVIA_CHECK(httpParseMultipartBoundary("text/plain; boundary=abc", boundary) ==
                HttpMultipartBoundaryStatus::kInvalidContentType);
    RUVIA_CHECK(httpParseMultipartBoundary("multipart/form-data", boundary) ==
                HttpMultipartBoundaryStatus::kInvalidBoundary);
    RUVIA_CHECK(httpParseMultipartBoundary("multipart/form-data; charset=utf-8", boundary) ==
                HttpMultipartBoundaryStatus::kInvalidBoundary);
}

// Part header (Content-Disposition / Content-Type) parsing and its error states.
RUVIA_TEST(multipart_part_headers_status_codes) {
    using ruvia::detail::httpParseMultipartPartHeaders;
    using ruvia::detail::HttpMultipartPartHeaders;
    using ruvia::detail::HttpMultipartPartHeaderStatus;

    HttpMultipartPartHeaders headers;
    RUVIA_CHECK(httpParseMultipartPartHeaders(
                    "Content-Disposition: form-data; name=\"field\"; filename=\"f.txt\"\r\n"
                    "Content-Type: text/plain",
                    headers) == HttpMultipartPartHeaderStatus::kOk);
    RUVIA_CHECK_EQ(headers.name, std::string_view("field"));
    RUVIA_CHECK_EQ(headers.filename, std::string_view("f.txt"));
    RUVIA_CHECK_EQ(headers.contentType, std::string_view("text/plain"));

    // form-data with no name parameter.
    HttpMultipartPartHeaders missingName;
    RUVIA_CHECK(httpParseMultipartPartHeaders("Content-Disposition: form-data", missingName) ==
                HttpMultipartPartHeaderStatus::kMissingName);

    // A non-form-data disposition, and no disposition at all, are invalid.
    HttpMultipartPartHeaders notFormData;
    RUVIA_CHECK(httpParseMultipartPartHeaders(
                    "Content-Disposition: attachment; name=\"x\"", notFormData) ==
                HttpMultipartPartHeaderStatus::kInvalidDisposition);
    HttpMultipartPartHeaders noDisposition;
    RUVIA_CHECK(httpParseMultipartPartHeaders("Content-Type: text/plain", noDisposition) ==
                HttpMultipartPartHeaderStatus::kInvalidDisposition);
}
