#include "test_harness.h"

#include <concepts>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <zstd.h>

#include "ruvia/http/detail/HeaderAcceptUtils.h"
#include "ruvia/http/detail/HeaderTokenUtils.h"
#include "ruvia/http/detail/parser/HttpChunkParser.h"
#include "ruvia/http/detail/MultipartParsing.h"
#include "ruvia/http/detail/RequestBodyDecoding.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/web/Model.h"

namespace {

using ruvia::detail::HttpContentCoding;
using ruvia::detail::HttpChunkScanComplete;
using ruvia::detail::HttpChunkScanFailure;
using ruvia::detail::HttpChunkScanNeedMore;
using ruvia::detail::HttpChunkScanResult;
using ruvia::detail::HttpMultipartPartHeaders;

template <typename T>
concept HasChunkScanConsumedBytes = requires(const T& result) {
    { result.consumedBytes() } -> std::same_as<std::size_t>;
};

template <typename T>
concept HasChunkScanError = requires(const T& result) {
    { result.error() } -> std::same_as<ruvia::detail::HttpChunkScanError>;
};

template <typename T>
concept HasAnyRvalueHttpChunkScanAccessor =
    requires(T&& result) { std::move(result).needMore(); } ||
    requires(T&& result) { std::move(result).complete(); } ||
    requires(T&& result) { std::move(result).failure(); };

static_assert(std::same_as<
    decltype(ruvia::detail::scanHttpChunkedBody(std::string_view{})),
    HttpChunkScanResult>);
static_assert(!std::default_initializable<HttpChunkScanResult>);
static_assert(!HasAnyRvalueHttpChunkScanAccessor<HttpChunkScanResult>);
static_assert(!HasChunkScanConsumedBytes<HttpChunkScanNeedMore>);
static_assert(HasChunkScanConsumedBytes<HttpChunkScanComplete>);
static_assert(!HasChunkScanConsumedBytes<HttpChunkScanFailure>);
static_assert(!HasChunkScanError<HttpChunkScanNeedMore>);
static_assert(!HasChunkScanError<HttpChunkScanComplete>);
static_assert(HasChunkScanError<HttpChunkScanFailure>);

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

template <typename T>
concept ParsesAnyRvalueOwningString =
    requires(std::string&& body) {
        T::parse(std::move(body), std::pmr::get_default_resource());
    } ||
    requires(const std::string&& body) {
        T::parse(std::move(body), std::pmr::get_default_resource());
    };

template <typename T>
concept ParsesLvalueOwningString = requires(const std::string& body) {
    T::parse(body, std::pmr::get_default_resource());
};

static_assert(!HasCookiesAccessor<ruvia::HttpRequest>);
static_assert(!HasQueryListAccessor<ruvia::HttpRequest>);
static_assert(!HasQueriesVectorAccessor<ruvia::HttpRequest>);
static_assert(!ParsesAnyRvalueOwningString<ruvia::JsonValue>);
static_assert(!ParsesAnyRvalueOwningString<ruvia::JsonObject>);
static_assert(!ParsesAnyRvalueOwningString<ruvia::FormObject>);
static_assert(ParsesLvalueOwningString<ruvia::JsonValue>);
static_assert(ParsesLvalueOwningString<ruvia::JsonObject>);
static_assert(ParsesLvalueOwningString<ruvia::FormObject>);

RUVIA_REQUEST_MODEL(AccessorSurfaceRequest,
    RUVIA_FIELD(message, ruvia::String)
);

RUVIA_RESPONSE_MODEL(AccessorSurfaceResponse,
    RUVIA_FIELD(message, ruvia::String)
);

template <typename T>
concept ExposesAnyRvalueGeneratedMessageMember =
    requires { std::declval<const T&&>().message(); } ||
    requires { std::declval<T&&>().messageEnsure(); } ||
    requires { std::declval<T&&>().message(std::string_view{}); };

static_assert(std::same_as<
    std::remove_cvref_t<decltype(std::declval<AccessorSurfaceRequest&>().message())>,
    std::optional<ruvia::String>>);
static_assert(std::same_as<
    std::remove_cvref_t<decltype(std::declval<const AccessorSurfaceRequest&>().message())>,
    std::optional<ruvia::String>>);
static_assert(!ExposesAnyRvalueGeneratedMessageMember<AccessorSurfaceRequest>);
static_assert(!ExposesAnyRvalueGeneratedMessageMember<AccessorSurfaceResponse>);

std::optional<std::string> zstdRoundTrip(std::string_view plain, std::size_t truncateBy) {
    const std::size_t bound = ZSTD_compressBound(plain.size());
    std::string compressed(bound, '\0');
    const std::size_t written = ZSTD_compress(
        compressed.data(), compressed.size(), plain.data(), plain.size(), 3);
    if (ZSTD_isError(written) != 0 || written <= truncateBy) {
        return std::nullopt;
    }
    compressed.resize(written - truncateBy);

    const auto result = ruvia::detail::decodeHttpContent(
        HttpContentCoding::kZstd,
        compressed,
        ruvia::kDefaultMaxBufferedBodyBytes,
        std::pmr::get_default_resource());
    const auto* decoded = result.decoded();
    if (decoded == nullptr) {
        return std::nullopt;
    }
    return std::string(decoded->bytes());
}

}  // namespace

RUVIA_TEST(model_factory_materializes_before_publication) {
    std::pmr::monotonic_buffer_resource modelResource;
    const auto parsed = ruvia::JsonBody<AccessorSurfaceRequest>::parse(
        R"({"message":"ready"})",
        &modelResource);
    RUVIA_CHECK(parsed.has_value());
    if (parsed.has_value()) {
        const AccessorSurfaceRequest& model = *parsed;
        RUVIA_CHECK(model.message().has_value());
        if (model.message().has_value()) {
            RUVIA_CHECK_EQ(model.message()->view(), std::string_view("ready"));
            RUVIA_CHECK(model.message()->resource() == &modelResource);
        }
        RUVIA_CHECK(
            ruvia::detail::ModelValidationAccess::fieldState<"message">(model) ==
            ruvia::detail::ModelFieldState::kParsed);
    }

    const auto invalidField = ruvia::JsonBody<AccessorSurfaceRequest>::parse(
        R"({"message":42})",
        std::pmr::get_default_resource());
    RUVIA_CHECK(invalidField.has_value());
    if (invalidField.has_value()) {
        RUVIA_CHECK(!invalidField->message().has_value());
        RUVIA_CHECK(
            ruvia::detail::ModelValidationAccess::fieldState<"message">(*invalidField) ==
            ruvia::detail::ModelFieldState::kInvalidType);
    }

    const auto malformed = ruvia::JsonBody<AccessorSurfaceRequest>::parse(
        R"({"message":"incomplete")",
        &modelResource);
    RUVIA_CHECK(!malformed.has_value());

    AccessorSurfaceResponse response(&modelResource);
    RUVIA_CHECK(response.messageEnsure().resource() == &modelResource);
}

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

RUVIA_TEST(form_object_get_uses_last_match_after_invalid_duplicate) {
    auto form = ruvia::FormObject::parse("age=nope&other=x&age=42", std::pmr::get_default_resource());
    RUVIA_CHECK(form.has_value());

    const auto value = form->get<ruvia::Int32>("age");
    RUVIA_CHECK(value.has_value());
    RUVIA_CHECK_EQ(static_cast<std::int32_t>(*value), 42);
}

RUVIA_TEST(json_object_get_uses_last_match) {
    auto json = ruvia::JsonObject::parse(R"({"name":"first","other":"x","name":"second"})", std::pmr::get_default_resource());
    RUVIA_CHECK(json.has_value());

    const auto value = json->get<ruvia::String>("name");
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
    const auto result = ruvia::detail::httpParseMultipartBoundary(contentType);
    RUVIA_CHECK(result.boundary() != nullptr);
    RUVIA_CHECK_EQ(result.boundary()->value(), std::string_view("a:b"));
}

// --- Multipart boundary must be a full delimiter line, not a prefix ------
RUVIA_TEST(multipart_boundary_prefix_requires_delimiter_terminator) {
    using ruvia::detail::httpFindMultipartBodyDelimiter;
    // "abc" appears as a substring of "abcXYZ" in the body; that is NOT a delimiter (a delimiter
    // must be followed by CRLF or "--"). The scan must skip the false match and find the real one.
    const std::string_view body = "data\r\n--abcXYZ tail\r\n--abc\r\n";
    const auto match = httpFindMultipartBodyDelimiter(
        body, ruvia::MultipartBoundary("abc"), true);
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
    const auto match = httpFindInitialMultipartDelimiter(
        body, ruvia::MultipartBoundary("abc"), true);
    const auto* part = match.part();
    RUVIA_CHECK(part != nullptr);
    if (part != nullptr) {
        RUVIA_CHECK_EQ(part->offset(), body.rfind("--abc\r\n"));
    }
    // A close delimiter ("--abc--") is a valid terminator too.
    const std::string_view closing = "--abc--\r\n";
    const auto closeMatch = httpFindInitialMultipartDelimiter(
        closing, ruvia::MultipartBoundary("abc"), true);
    RUVIA_CHECK(closeMatch.close() != nullptr);
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

// --- Chunk extension quoted-pair follows RFC quoted-string grammar -------
RUVIA_TEST(chunk_extension_quoted_pair_allows_escaped_htab) {
    using ruvia::detail::scanHttpChunkedBody;

    const std::string_view body = "1;note=\"a\\\tb\"\r\nx\r\n0\r\n\r\n";
    const auto result = scanHttpChunkedBody(body);
    RUVIA_CHECK(result.complete() != nullptr);
    RUVIA_CHECK_EQ(result.complete()->consumedBytes(), body.size());
    RUVIA_CHECK(result.needMore() == nullptr);
    RUVIA_CHECK(result.failure() == nullptr);
}

RUVIA_TEST(chunk_scan_result_is_discriminated) {
    const auto needMore = ruvia::detail::scanHttpChunkedBody("1\r\nx");
    RUVIA_CHECK(needMore.needMore() != nullptr);
    RUVIA_CHECK(needMore.complete() == nullptr);
    RUVIA_CHECK(needMore.failure() == nullptr);

    const auto failure = ruvia::detail::scanHttpChunkedBody("xyz\r\n");
    RUVIA_CHECK(failure.failure() != nullptr);
    RUVIA_CHECK(
        failure.failure()->error() ==
        ruvia::detail::HttpChunkScanError::kInvalidSize);
    RUVIA_CHECK(failure.needMore() == nullptr);
    RUVIA_CHECK(failure.complete() == nullptr);
}

RUVIA_TEST(chunk_trailer_section_enforces_field_and_byte_limits) {
    std::string tooMany = "0\r\n";
    for (std::size_t i = 0; i <= ruvia::kMaxHttpHeaderFields; ++i) {
        tooMany.append("X-Trace: value\r\n");
    }
    tooMany.append("\r\n");
    const auto tooManyResult = ruvia::detail::scanHttpChunkedBody(tooMany);
    RUVIA_CHECK(tooManyResult.failure() != nullptr);
    if (const auto* failure = tooManyResult.failure()) {
        RUVIA_CHECK(
            failure->error() ==
            ruvia::detail::HttpChunkScanError::kTooLarge);
    }

    std::string oversized = "0\r\nX-Trace: ";
    oversized.append(ruvia::kMaxHttpHeaderBytes, 'x');
    oversized.append("\r\n\r\n");
    const auto oversizedResult =
        ruvia::detail::scanHttpChunkedBody(oversized);
    RUVIA_CHECK(oversizedResult.failure() != nullptr);
    if (const auto* failure = oversizedResult.failure()) {
        RUVIA_CHECK(
            failure->error() ==
            ruvia::detail::HttpChunkScanError::kTooLarge);
    }
}

RUVIA_TEST(chunk_scan_rejects_unterminated_framing_at_the_header_limit) {
    std::string oversizedSizeLine(ruvia::kMaxHttpHeaderBytes, '1');
    const auto sizeLineResult =
        ruvia::detail::scanHttpChunkedBody(oversizedSizeLine);
    RUVIA_CHECK(sizeLineResult.failure() != nullptr);
    if (const auto* failure = sizeLineResult.failure()) {
        RUVIA_CHECK(
            failure->error() ==
            ruvia::detail::HttpChunkScanError::kTooLarge);
    }

    std::string oversizedTerminatedSizeLine = "1;x=";
    oversizedTerminatedSizeLine.append(
        ruvia::kMaxHttpHeaderBytes, 'a');
    oversizedTerminatedSizeLine.append("\r\n");
    const auto terminatedSizeLineResult =
        ruvia::detail::scanHttpChunkedBody(oversizedTerminatedSizeLine);
    RUVIA_CHECK(terminatedSizeLineResult.failure() != nullptr);
    if (const auto* failure = terminatedSizeLineResult.failure()) {
        RUVIA_CHECK(
            failure->error() ==
            ruvia::detail::HttpChunkScanError::kTooLarge);
    }

    std::string oversizedTrailers = "0\r\nX-Trace: ";
    oversizedTrailers.append(ruvia::kMaxHttpHeaderBytes, 'x');
    const auto trailerResult =
        ruvia::detail::scanHttpChunkedBody(oversizedTrailers);
    RUVIA_CHECK(trailerResult.failure() != nullptr);
    if (const auto* failure = trailerResult.failure()) {
        RUVIA_CHECK(
            failure->error() ==
            ruvia::detail::HttpChunkScanError::kTooLarge);
    }

    std::string boundarySizeLine = "1;x=";
    boundarySizeLine.append(
        ruvia::kMaxHttpHeaderBytes - boundarySizeLine.size() - 2,
        'a');
    boundarySizeLine.append("\r\nx\r\n0\r\n\r\n");
    const auto boundaryResult =
        ruvia::detail::scanHttpChunkedBody(boundarySizeLine);
    RUVIA_CHECK(boundaryResult.complete() != nullptr);
}
