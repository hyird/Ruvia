#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <zstd.h>

#include "http/HeaderAcceptUtils.h"
#include "http/HeaderTokenUtils.h"
#include "http/parser/HttpChunkParser.h"
#include "http/MultipartParsing.h"
#include "http/RequestBodyDecoding.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/Model.h"

namespace {

using ruvia::detail::HttpContentCoding;
using ruvia::detail::HttpMultipartPartHeaderStatus;
using ruvia::detail::HttpMultipartPartHeaders;

template <typename T>
concept HasCookiesAccessor = requires(const T& request) {
    request.cookies();
};

template <typename T>
concept HasQueryListAccessor = requires(const T& request) {
    request.query();
};

template <typename T>
concept HasQueriesVectorAccessor = requires(const T& request) {
    request.queries(std::string_view{});
};

static_assert(!HasCookiesAccessor<ruvia::HttpRequest>);
static_assert(!HasQueryListAccessor<ruvia::HttpRequest>);
static_assert(!HasQueriesVectorAccessor<ruvia::HttpRequest>);

RUVIA_MODEL(AccessorSurfaceModel,
    RUVIA_FIELD(message, ruvia::String)
);

static_assert(std::same_as<
    std::remove_cvref_t<decltype(std::declval<AccessorSurfaceModel&>().message())>,
    std::optional<ruvia::String>>);
static_assert(std::same_as<
    std::remove_cvref_t<decltype(std::declval<const AccessorSurfaceModel&>().message())>,
    std::optional<ruvia::String>>);

std::optional<std::string> zstdRoundTrip(std::string_view plain, std::size_t truncateBy) {
    const std::size_t bound = ZSTD_compressBound(plain.size());
    std::string compressed(bound, '\0');
    const std::size_t written = ZSTD_compress(
        compressed.data(), compressed.size(), plain.data(), plain.size(), 3);
    if (ZSTD_isError(written) != 0 || written <= truncateBy) {
        return std::nullopt;
    }
    compressed.resize(written - truncateBy);

    std::pmr::string out(std::pmr::get_default_resource());
    if (!ruvia::detail::decodeRequestContentEncoding(
            HttpContentCoding::kZstd,
            compressed,
            out,
            ruvia::detail::kMaxDecodedRequestBodyBytes)) {
        return std::nullopt;
    }
    return std::string(out.c_str(), out.size());
}

}  // namespace

// --- Semicolon parameters: quoted-string awareness -----------------------
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

RUVIA_TEST(form_object_get_uses_last_match) {
    auto form = ruvia::FormObject::parse("name=first&other=x&name=second", std::pmr::get_default_resource());
    RUVIA_CHECK(form.has_value());

    const auto value = form->get<ruvia::String>("name");
    RUVIA_CHECK(value.has_value());
    RUVIA_CHECK_EQ(value->view(), std::string_view("second"));
}

RUVIA_TEST(semicolon_params_quoted_uses_last_match) {
    using ruvia::detail::httpFindSemicolonParameterQuoted;
    const std::string_view v = R"(form-data; name="first"; filename=a.txt; name="second")";
    RUVIA_CHECK_EQ(
        httpFindSemicolonParameterQuoted(v, "name").value_or("?"),
        std::string_view(R"("second")"));
}

RUVIA_TEST(multipart_part_headers_quoted_name_with_semicolon) {
    HttpMultipartPartHeaders headers;
    const std::string_view block =
        "Content-Disposition: form-data; name=\"a;b\"; filename=\"up;load.txt\"\r\n"
        "Content-Type: text/plain";
    const auto status = ruvia::detail::httpParseMultipartPartHeaders(block, headers);
    RUVIA_CHECK(status == HttpMultipartPartHeaderStatus::kOk);
    RUVIA_CHECK_EQ(headers.name, std::string_view("a;b"));
    RUVIA_CHECK_EQ(headers.filename, std::string_view("up;load.txt"));
    RUVIA_CHECK_EQ(headers.contentType, std::string_view("text/plain"));
}

RUVIA_TEST(multipart_boundary_quoted_with_semicolon) {
    std::string_view boundary;
    const std::string_view contentType = R"(multipart/form-data; boundary="a;b")";
    const auto status = ruvia::detail::httpParseMultipartBoundary(contentType, boundary);
    RUVIA_CHECK(status == ruvia::detail::HttpMultipartBoundaryStatus::kOk);
    RUVIA_CHECK_EQ(boundary, std::string_view("a;b"));
}

// --- Multipart boundary must be a full delimiter line, not a prefix ------
RUVIA_TEST(multipart_boundary_prefix_requires_delimiter_terminator) {
    using ruvia::detail::httpFindMultipartBoundaryPrefix;
    // "abc" appears as a substring of "abcXYZ" in the body; that is NOT a delimiter (a delimiter
    // must be followed by CRLF or "--"). The scan must skip the false match and find the real one.
    const std::string_view body = "data\r\n--abcXYZ tail\r\n--abc\r\n";
    RUVIA_CHECK_EQ(
        httpFindMultipartBoundaryPrefix(body, "abc"),
        body.find("\r\n--abc\r\n"));
}

RUVIA_TEST(multipart_boundary_line_requires_delimiter_terminator) {
    using ruvia::detail::httpFindMultipartBoundaryLine;
    // Same for the opening delimiter: "--abcXYZ" is not the boundary line; "--abc\r\n" is.
    const std::string_view body = "--abcXYZ junk--abc\r\nrest";
    RUVIA_CHECK_EQ(
        httpFindMultipartBoundaryLine(body, "abc"),
        body.find("--abc\r\n"));
    // A close delimiter ("--abc--") is a valid terminator too.
    const std::string_view closing = "--abc--\r\n";
    RUVIA_CHECK_EQ(httpFindMultipartBoundaryLine(closing, "abc"), std::size_t{0});
}

// --- zstd request-body decode: truncation must be rejected ---------------
RUVIA_TEST(zstd_decode_full_frame_succeeds) {
    const std::string plain(4096, 'z');  // compressible payload spanning a block
    const auto decoded = zstdRoundTrip(plain, 0);
    RUVIA_CHECK(decoded.has_value());
    if (decoded) {
        RUVIA_CHECK_EQ(*decoded, plain);
    }
}

RUVIA_TEST(zstd_decode_truncated_frame_rejected) {
    const std::string plain(4096, 'z');
    // Dropping the final bytes yields an incomplete frame that must be rejected,
    // matching the zlib/brotli decoders (regression guard for silent-truncation).
    RUVIA_CHECK(!zstdRoundTrip(plain, 4).has_value());
}

// --- Accept quality parsing shares the quote-aware parameter scanner ------
RUVIA_TEST(accept_quality_quoted_semicolon_param) {
    using ruvia::detail::httpAcceptsMediaType;
    // A ';' inside a quoted media-range parameter must NOT be read as a parameter
    // separator when locating q (RFC 7231 §5.3.2). Before unifying onto the quote-aware
    // scanner this mis-read "q=0" from inside the quotes and rejected the type.
    RUVIA_CHECK(httpAcceptsMediaType(
        R"(application/json;version="a;q=0";q=0.9)", "application/json"));
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

// --- Chunk extension quoted-pair follows RFC quoted-string grammar -------
RUVIA_TEST(chunk_extension_quoted_pair_allows_escaped_htab) {
    using ruvia::detail::HttpChunkScanStatus;
    using ruvia::detail::scanHttpChunkedBody;

    const std::string_view body = "1;note=\"a\\\tb\"\r\nx\r\n0\r\n\r\n";
    const auto result = scanHttpChunkedBody(body);
    RUVIA_CHECK(result.status == HttpChunkScanStatus::kComplete);
    RUVIA_CHECK_EQ(result.consumedBytes, body.size());
}
