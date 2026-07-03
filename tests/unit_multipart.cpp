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
