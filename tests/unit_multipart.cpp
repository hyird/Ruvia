#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/detail/HttpCommonInternal.h"
#include "ruvia/http/detail/MultipartParsing.h"

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

RUVIA_TEST(multipart_boundary_prefix_of_longer_token_is_not_a_delimiter) {
    using ruvia::detail::httpFindMultipartBoundaryLine;
    using ruvia::detail::httpFindMultipartBoundaryPrefix;
    // The boundary "abc" appearing as a strict PREFIX of a longer token ("abcXYZ")
    // must not be mistaken for a delimiter: the byte after the boundary is a letter,
    // which is neither the CRLF of a next-part delimiter nor the "--" of a close
    // delimiter. This is the default-reject branch of httpMultipartBoundaryAt,
    // distinct from the '-'-not-followed-by-'-' case above -- and the core defense
    // against a field value that merely starts with the boundary string splitting
    // the stream at the wrong place. The real delimiter is the later "\r\n--abc\r\n".
    const std::string_view body = "\r\n--abcXYZ\r\n--abc\r\n";
    RUVIA_CHECK_EQ(httpFindMultipartBoundaryPrefix(body, "abc"), body.find("\r\n--abc\r\n"));
    // Same for the line form ("--" prefix, no leading CRLF): "--abcXYZ" is skipped.
    const std::string_view line = "--abcXYZ\r\n--abc\r\n";
    RUVIA_CHECK_EQ(httpFindMultipartBoundaryLine(line, "abc"), line.find("--abc\r\n", 1));
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
    // A present-but-empty boundary value is rejected too (a distinct branch from a
    // missing parameter): an empty delimiter would match at every "--" in the body.
    RUVIA_CHECK(httpParseMultipartBoundary("multipart/form-data; boundary=", boundary) ==
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

RUVIA_TEST(multipart_part_header_names_are_case_insensitive) {
    using ruvia::detail::httpParseMultipartPartHeaders;
    using ruvia::detail::HttpMultipartPartHeaders;
    using ruvia::detail::HttpMultipartPartHeaderStatus;

    // HTTP field names are case-insensitive; a part that lowercases them (some
    // clients do) must still be recognized, with name and content type extracted.
    HttpMultipartPartHeaders headers;
    RUVIA_CHECK(httpParseMultipartPartHeaders(
                    "content-disposition: form-data; name=\"field\"\r\n"
                    "content-type: image/png",
                    headers) == HttpMultipartPartHeaderStatus::kOk);
    RUVIA_CHECK_EQ(headers.name, std::string_view("field"));
    RUVIA_CHECK_EQ(headers.contentType, std::string_view("image/png"));
}

RUVIA_TEST(multipart_header_value_in_block_lookup) {
    using ruvia::detail::httpHeaderValueInBlock;
    const std::string_view block =
        "Content-Disposition: form-data; name=\"a\"\r\n"
        "Content-Type: text/plain";
    // Case-insensitive name match with OWS-trimmed value; the last line has no
    // trailing CRLF and must still be found.
    RUVIA_CHECK(httpHeaderValueInBlock(block, "content-type") == std::string_view("text/plain"));
    RUVIA_CHECK(httpHeaderValueInBlock(block, "CONTENT-TYPE") == std::string_view("text/plain"));
    RUVIA_CHECK(httpHeaderValueInBlock(block, "Content-Disposition") ==
                std::string_view("form-data; name=\"a\""));
    // Missing header -> nullopt.
    RUVIA_CHECK(!httpHeaderValueInBlock(block, "X-Absent").has_value());
    // A line without a colon is skipped, not matched by name.
    RUVIA_CHECK(!httpHeaderValueInBlock("garbageline\r\nX: v", "garbageline").has_value());
    // Surrounding OWS on the value is trimmed.
    RUVIA_CHECK(httpHeaderValueInBlock("X:   spaced   ", "X") == std::string_view("spaced"));
}

RUVIA_TEST(multipart_header_value_in_block_uses_last_match) {
    using ruvia::detail::httpHeaderValueInBlock;
    const std::string_view block =
        "Content-Type: text/plain\r\n"
        "X-Other: value\r\n"
        "content-type: image/png";

    RUVIA_CHECK(httpHeaderValueInBlock(block, "Content-Type") == std::string_view("image/png"));
}

RUVIA_TEST(multipart_disposition_parameter_extraction) {
    using ruvia::detail::httpDispositionParameter;
    const std::string_view disposition = "form-data; name=\"field\"; filename=\"a.txt\"";
    RUVIA_CHECK(httpDispositionParameter(disposition, "name") == std::string_view("field"));
    RUVIA_CHECK(httpDispositionParameter(disposition, "filename") == std::string_view("a.txt"));
    // An unquoted parameter value is returned as-is.
    RUVIA_CHECK(httpDispositionParameter("form-data; name=plain", "name") == std::string_view("plain"));
    // An absent parameter is nullopt.
    RUVIA_CHECK(!httpDispositionParameter(disposition, "charset").has_value());
    // Parameter names are case-insensitive (RFC 6266 §4.1), like the Content-Type
    // boundary parameter -- `Name`/`FileName` must resolve, not be rejected.
    const std::string_view mixedCase = "form-data; Name=\"field\"; FileName=\"a.txt\"";
    RUVIA_CHECK(httpDispositionParameter(mixedCase, "name") == std::string_view("field"));
    RUVIA_CHECK(httpDispositionParameter(mixedCase, "filename") == std::string_view("a.txt"));
}

RUVIA_TEST(multipart_is_form_data_disposition) {
    using ruvia::detail::httpIsFormDataDisposition;
    RUVIA_CHECK(httpIsFormDataDisposition("form-data; name=\"x\""));
    RUVIA_CHECK(httpIsFormDataDisposition("FORM-DATA"));           // case-insensitive
    RUVIA_CHECK(httpIsFormDataDisposition("  form-data  ; filename=\"y\""));  // OWS-trimmed type
    RUVIA_CHECK(!httpIsFormDataDisposition("attachment; name=\"x\""));
    RUVIA_CHECK(!httpIsFormDataDisposition("form-data-extra"));    // whole type compared
    RUVIA_CHECK(!httpIsFormDataDisposition(""));
}

RUVIA_TEST(multipart_part_access_decodes_quoted_pairs) {
    // The buffered parser builds parts via MultipartPartAccess::make, which must
    // decode RFC 7230 §3.2.6 quoted-pairs in name/filename (they are part-owned so
    // they may differ from the raw request bytes); contentType/body stay verbatim.
    auto* resource = std::pmr::get_default_resource();
    const auto part = ruvia::detail::MultipartPartAccess::make(
        "a\\\"b", "x\\\\y.txt", "text/plain", "the body", resource);
    RUVIA_CHECK_EQ(std::string(part.name()), std::string("a\"b"));
    RUVIA_CHECK_EQ(std::string(part.filename()), std::string("x\\y.txt"));
    RUVIA_CHECK_EQ(std::string(part.contentType()), std::string("text/plain"));
    RUVIA_CHECK_EQ(std::string(part.body()), std::string("the body"));
}
