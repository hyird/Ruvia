#include "test_harness.h"

#include <concepts>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/http/Http1RequestParser.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpParseError.h"
#include "ruvia/http/HttpRequest.h"

namespace {

using ruvia::HttpKnownMethod;
using ruvia::HttpParseError;
using ruvia::HttpProtocolVersion;
using ruvia::detail::Http1ServerRequestParseFailureSource;
using ruvia::detail::Http1ServerRequestParser;
using ruvia::detail::Http1ServerRequestParseState;
using ruvia::detail::HttpUnsupportedExpectationPolicy;

template <typename T>
concept HasAnyRvalueHttp1RequestParseAccessor =
    requires(T&& result) { std::move(result).needMore(); } ||
    requires(T&& result) { std::move(result).parsed(); } ||
    requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasAnyRvalueHttp1ParsedRequestBorrow =
    requires(T&& parsed) { std::move(parsed).request(); } ||
    requires(T&& parsed) { std::move(parsed).bodyPlan(); };

static_assert(!HasAnyRvalueHttp1RequestParseAccessor<
    ruvia::Http1RequestParseResult>);
static_assert(!HasAnyRvalueHttp1ParsedRequestBorrow<
    ruvia::Http1ParsedRequest>);

const ruvia::detail::Http1KnownLengthRequestBody& requireKnownLength(
    const ruvia::detail::Http1RequestBodyPlan& plan) {
    const auto* knownLength = plan.knownLength();
    if (knownLength == nullptr) {
        throw std::runtime_error("test expected known-length request framing");
    }
    return *knownLength;
}

const ruvia::detail::Http1ChunkedRequestBody& requireChunked(
    const ruvia::detail::Http1RequestBodyPlan& plan) {
    const auto* chunked = plan.chunked();
    if (chunked == nullptr) {
        throw std::runtime_error("test expected chunked request framing");
    }
    return *chunked;
}

[[nodiscard]] bool isFailure(
    const Http1ServerRequestParseState& state,
    HttpParseError error) noexcept {
    const auto* failure = state.failure();
    if (failure == nullptr) {
        return false;
    }
    const auto actual = failure->protocolError();
    const auto expected = ruvia::httpParseProtocolError(error);
    return actual.status() == expected.status() &&
        std::string_view(actual.what()) == expected.what();
}

}  // namespace

RUVIA_TEST(http1_public_parse_outcome_exposes_only_its_active_alternative) {
    const ruvia::Http1RequestParser publicParser;

    const auto needMore = publicParser.parse(
        "GET / HTTP/1.1\r\nHost: example.com\r\n");
    const auto* needMoreState = needMore.needMore();
    RUVIA_CHECK(needMoreState != nullptr);
    if (needMoreState != nullptr) {
        RUVIA_CHECK(!needMoreState->requiredTotalBytes().has_value());
    }
    RUVIA_CHECK(needMore.parsed() == nullptr);
    RUVIA_CHECK(needMore.failure() == nullptr);

    const auto failure = publicParser.parse("GET / HTTP/1.1\r\n\r\n");
    const auto* failureState = failure.failure();
    RUVIA_CHECK(failure.needMore() == nullptr);
    RUVIA_CHECK(failure.parsed() == nullptr);
    RUVIA_CHECK(failureState != nullptr);
    if (failureState != nullptr) {
        const auto error = failureState->protocolError();
        RUVIA_CHECK_EQ(error.status(), 400);
        RUVIA_CHECK_EQ(
            std::string_view(error.what()),
            std::string_view("missing Host header"));
    }
}

RUVIA_TEST(http1_internal_parse_failure_classifies_only_request_line_failures) {
    Http1ServerRequestParser parser;

    const auto requestLineFailure = parser.parseMessage(
        "GET / HTTP/9.0\r\nHost: example.com\r\n\r\n");
    RUVIA_CHECK(requestLineFailure.failure() != nullptr);
    if (const auto* failure = requestLineFailure.failure()) {
        RUVIA_CHECK(
            failure->source() ==
            Http1ServerRequestParseFailureSource::kRequestLine);
    }

    const auto messageFailure = parser.parseMessage("GET / HTTP/1.1\r\n\r\n");
    RUVIA_CHECK(messageFailure.failure() != nullptr);
    if (const auto* failure = messageFailure.failure()) {
        RUVIA_CHECK(
            failure->source() ==
            Http1ServerRequestParseFailureSource::kMessage);
    }
}

RUVIA_TEST(http1_public_parse_need_more_separates_required_size_from_consumption) {
    const ruvia::Http1RequestParser publicParser;
    constexpr std::string_view partial =
        "POST / HTTP/1.1\r\nHost: example.com\r\n"
        "Content-Length: 5\r\n\r\nhe";
    const auto result = publicParser.parse(partial);
    const auto* needMore = result.needMore();
    RUVIA_CHECK(needMore != nullptr);
    if (needMore != nullptr) {
        RUVIA_CHECK(needMore->requiredTotalBytes().has_value());
        if (needMore->requiredTotalBytes()) {
            RUVIA_CHECK_EQ(
                *needMore->requiredTotalBytes(),
                partial.size() + std::size_t{3});
        }
    }
}

RUVIA_TEST(http1_public_parse_success_retains_the_exact_framed_body) {
    const ruvia::Http1RequestParser publicParser;

    constexpr std::string_view contentLengthMessage =
        "POST /fixed HTTP/1.1\r\nHost: example.com\r\n"
        "Content-Length: 5\r\n\r\nhello";
    const std::string contentLengthPipeline =
        std::string(contentLengthMessage) +
        "GET /next HTTP/1.1\r\nHost: example.com\r\n\r\n";
    const auto fixedResult = publicParser.parse(contentLengthPipeline);
    const auto* fixed = fixedResult.parsed();
    RUVIA_CHECK(fixed != nullptr);
    if (fixed != nullptr) {
        RUVIA_CHECK_EQ(fixed->request().path(), std::string_view("/fixed"));
        RUVIA_CHECK_EQ(
            requireKnownLength(fixed->bodyPlan()).contentLength(),
            std::size_t{5});
        RUVIA_CHECK_EQ(fixed->wireBody(), std::string_view("hello"));
        RUVIA_CHECK_EQ(fixed->consumedBytes(), contentLengthMessage.size());
    }

    constexpr std::string_view chunkedMessage =
        "POST /chunked HTTP/1.1\r\nHost: example.com\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "3\r\nabc\r\n0\r\nX-Checksum: ok\r\n\r\n";
    const std::string chunkedPipeline =
        std::string(chunkedMessage) +
        "GET /next HTTP/1.1\r\nHost: example.com\r\n\r\n";
    const auto chunkedResult = publicParser.parse(chunkedPipeline);
    const auto* chunked = chunkedResult.parsed();
    RUVIA_CHECK(chunked != nullptr);
    if (chunked != nullptr) {
        RUVIA_CHECK(chunked->bodyPlan().chunked() != nullptr);
        RUVIA_CHECK_EQ(
            chunked->wireBody(),
            std::string_view("3\r\nabc\r\n0\r\nX-Checksum: ok\r\n\r\n"));
        RUVIA_CHECK_EQ(chunked->consumedBytes(), chunkedMessage.size());
    }
}

RUVIA_TEST(http1_public_parser_preserves_expect_extensions_as_semantics) {
    const auto result = ruvia::Http1RequestParser().parse(
        "POST /extensions HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Expect: , 100-Continue, custom-feature,\r\n"
        "Content-Length: 1\r\n\r\nx");

    const auto* parsed = result.parsed();
    RUVIA_CHECK(parsed != nullptr);
    RUVIA_CHECK(result.failure() == nullptr);
    if (parsed != nullptr) {
        const auto plan = parsed->bodyPlan().expectationPlan(
            HttpUnsupportedExpectationPolicy::kReject);
        RUVIA_CHECK(plan.rejection() != nullptr);
        RUVIA_CHECK(parsed->bodyPlan().expectations().has100Continue());
        RUVIA_CHECK(parsed->bodyPlan().expectations().hasUnsupported());
    }
}

RUVIA_TEST(http1_parse_valid_request) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage("GET /path?q=1 HTTP/1.1\r\nHost: example.com\r\n\r\n");
    RUVIA_CHECK(result.messageReady());
    RUVIA_CHECK_EQ(result.request.method(), std::string_view("GET"));
    RUVIA_CHECK(result.request.knownMethod() == HttpKnownMethod::kGet);
    RUVIA_CHECK_EQ(result.request.path(), std::string_view("/path"));
    RUVIA_CHECK_EQ(result.request.queryString(), std::string_view("q=1"));
    RUVIA_CHECK_EQ(result.request.header("host"), std::string_view("example.com"));
    RUVIA_CHECK(
        result.request.protocolVersion() == HttpProtocolVersion::kHttp11);
}

RUVIA_TEST(http1_parser_maps_wire_versions_to_typed_control_data) {
    Http1ServerRequestParser parser;
    const auto http11 = parser.parseMessage(
        "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n");
    const auto http10 = parser.parseMessage("GET / HTTP/1.0\r\n\r\n");

    RUVIA_CHECK(http11.messageReady());
    RUVIA_CHECK(http10.messageReady());
    RUVIA_CHECK(
        http11.request.protocolVersion() == HttpProtocolVersion::kHttp11);
    RUVIA_CHECK(
        http10.request.protocolVersion() == HttpProtocolVersion::kHttp10);
}

RUVIA_TEST(http1_parse_incomplete_head) {
    Http1ServerRequestParser parser;
    // No terminating blank line yet -> incomplete, keep reading.
    const auto result = parser.parseMessage("GET / HTTP/1.1\r\nHost: example.com\r\n");
    RUVIA_CHECK(result.needRequestHead() != nullptr);
}

RUVIA_TEST(http1_parse_missing_host_rejected) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage("GET / HTTP/1.1\r\n\r\n");
    RUVIA_CHECK(isFailure(result, HttpParseError::kMissingHost));
}

RUVIA_TEST(http1_parse_rejects_header_smuggling_vectors) {
    const auto rejected = [](std::string_view request) {
        Http1ServerRequestParser parser;
        return parser.parseMessage(request).failure() != nullptr;
    };

    // obs-fold: a continuation line (leading SP or HTAB) must be rejected, not
    // folded into the previous value -- a classic request-smuggling vector
    // (RFC 9112 5.2 deprecates obs-fold in requests).
    RUVIA_CHECK(rejected("GET / HTTP/1.1\r\nHost: x\r\nX-Foo: bar\r\n baz\r\n\r\n"));
    RUVIA_CHECK(rejected("GET / HTTP/1.1\r\nHost: x\r\nX-Foo: bar\r\n\tbaz\r\n\r\n"));

    // Whitespace between the field name and the colon is rejected (it causes a
    // parsing differential across proxies that enables smuggling).
    RUVIA_CHECK(rejected("GET / HTTP/1.1\r\nHost: x\r\nX-Foo : bar\r\n\r\n"));

    // A bare LF or bare CR inside a field value cannot smuggle a second header
    // line: the value stops at the control byte and the required CRLF is absent.
    RUVIA_CHECK(rejected("GET / HTTP/1.1\r\nHost: x\r\nX-Foo: a\nb\r\n\r\n"));
    RUVIA_CHECK(rejected("GET / HTTP/1.1\r\nHost: x\r\nX-Foo: a\rb\r\n\r\n"));
}

RUVIA_TEST(http1_parse_invalid_request_line_rejected) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage("!!!garbage!!!\r\n\r\n");
    RUVIA_CHECK(result.failure() != nullptr);
}

RUVIA_TEST(http1_parse_content_length_body) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello");
    RUVIA_CHECK(result.messageReady());
    RUVIA_CHECK_EQ(result.request.method(), std::string_view("POST"));
    RUVIA_CHECK(result.request.knownMethod() == HttpKnownMethod::kPost);
    RUVIA_CHECK_EQ(
        requireKnownLength(result.bodyPlan).contentLength(),
        std::size_t{5});
}

RUVIA_TEST(http1_parse_conflicting_content_length_rejected) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n");
    RUVIA_CHECK(isFailure(result, HttpParseError::kConflictingContentLength));
}

RUVIA_TEST(http1_parse_content_length_combined_equal_values) {
    // RFC 9112 section 6.3 explicitly permits a comma-combined field when every
    // decimal value is valid and identical. The shared parser must consume the
    // whole list rather than selecting one attacker-controlled member.
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Content-Length: 5, 5\r\n\r\nhello");
    RUVIA_CHECK(result.messageReady());
    RUVIA_CHECK_EQ(
        requireKnownLength(result.bodyPlan).contentLength(),
        std::size_t{5});

    const auto conflict = parser.parseMessage(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Content-Length: 5, 6\r\n\r\n");
    RUVIA_CHECK(isFailure(conflict, HttpParseError::kConflictingContentLength));
}

RUVIA_TEST(http1_parse_invalid_content_length_forms_rejected) {
    // Signs, non-decimal notation, trailing junk, and empty combined members are
    // invalid; none can degrade into prefix parsing.
    for (const auto* value : {"+5", "-5", "0x10", "5abc", "5,", ",5"}) {
        Http1ServerRequestParser parser;
        const std::string raw =
            std::string("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: ") + value + "\r\n\r\n";
        const auto result = parser.parseMessage(raw);
        RUVIA_CHECK(isFailure(result, HttpParseError::kInvalidContentLength));
    }
}

RUVIA_TEST(http1_parse_identical_duplicate_content_length_accepted) {
    // RFC 9112 section 6.3: multiple Content-Length fields carrying the SAME value are not
    // a conflict -- only differing values are (rejected above). Pin the accept side
    // so a refactor can neither start rejecting all duplicates (breaking clients that
    // legitimately repeat the header) nor, worse, start accepting differing ones
    // (which would reopen the CL-vs-CL smuggling hole).
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello");
    RUVIA_CHECK(result.messageReady());
    RUVIA_CHECK_EQ(
        requireKnownLength(result.bodyPlan).contentLength(),
        std::size_t{5});
}

RUVIA_TEST(http1_parse_content_length_with_transfer_encoding_rejected) {
    // TE + CL together is a request-smuggling vector and must be rejected
    // (RFC 9112 section 6.3).
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n");
    RUVIA_CHECK(result.failure() != nullptr);
}

RUVIA_TEST(http1_parse_duplicate_content_type_rejected) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 2\r\n\r\n{}");
    RUVIA_CHECK(isFailure(result, HttpParseError::kInvalidHeader));
}

RUVIA_TEST(http1_parse_duplicate_range_rejected) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "GET /file HTTP/1.1\r\nHost: x\r\n"
        "Range: bytes=0-99\r\n"
        "Range: bytes=200-299\r\n\r\n");
    RUVIA_CHECK(isFailure(result, HttpParseError::kInvalidHeader));
}

RUVIA_TEST(http1_parse_repeated_etag_list_fields_accepted) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "GET /file HTTP/1.1\r\nHost: x\r\n"
        "If-None-Match: \"old\"\r\n"
        "If-None-Match: \"new\"\r\n\r\n");
    RUVIA_CHECK(result.messageReady());
}

RUVIA_TEST(http1_parse_duplicate_authorization_rejected) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "Authorization: Bearer first\r\n"
        "Authorization: Bearer second\r\n\r\n");
    RUVIA_CHECK(isFailure(result, HttpParseError::kInvalidHeader));
}

RUVIA_TEST(http1_parse_chunked_body) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
    RUVIA_CHECK(result.messageReady());
    RUVIA_CHECK(requireChunked(result.bodyPlan).transferCodings().empty());
}

RUVIA_TEST(http1_parse_transfer_coding_before_final_chunked) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Transfer-Encoding: gzip, chunked\r\n\r\n"
        "3\r\nraw\r\n0\r\n\r\n");
    RUVIA_CHECK(result.messageReady());
    const auto& chunked = requireChunked(result.bodyPlan);
    RUVIA_CHECK_EQ(chunked.transferCodings().count, std::size_t{1});
    RUVIA_CHECK(
        chunked.transferCodings().values[0] ==
        ruvia::detail::HttpTransferCoding::kGzip);
}

RUVIA_TEST(http1_parse_unsupported_version_rejected) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage("GET / HTTP/2.0\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(isFailure(result, HttpParseError::kUnsupportedHttpVersion));
}

RUVIA_TEST(http1_parse_extension_method_preserves_case_sensitive_token) {
    Http1ServerRequestParser parser;
    const auto extension = parser.parseMessage(
        "PROPFIND /dav HTTP/1.1\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(extension.messageReady());
    RUVIA_CHECK_EQ(extension.request.method(), std::string_view("PROPFIND"));
    RUVIA_CHECK(extension.request.knownMethod() == HttpKnownMethod::kUnknown);
    RUVIA_CHECK_EQ(extension.request.path(), std::string_view("/dav"));

    const auto lowercase = parser.parseMessage(
        "get /case-sensitive HTTP/1.1\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(lowercase.messageReady());
    RUVIA_CHECK_EQ(lowercase.request.method(), std::string_view("get"));
    RUVIA_CHECK(lowercase.request.knownMethod() == HttpKnownMethod::kUnknown);
}

RUVIA_TEST(http1_parse_invalid_method_token_rejected) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "BAD(METHOD / HTTP/1.1\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(isFailure(result, HttpParseError::kInvalidRequestLine));
}

RUVIA_TEST(http1_parse_transfer_encoding_not_chunked_rejected) {
    // A non-chunked Transfer-Encoding leaves message framing undetermined.
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\n\r\n");
    RUVIA_CHECK(isFailure(result, HttpParseError::kInvalidTransferEncoding));
}

RUVIA_TEST(http1_parse_transfer_encoding_in_http10_rejected) {
    // Transfer-Encoding in HTTP/1.0 is faulty framing (RFC 9112 6.1).
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "POST / HTTP/1.0\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n");
    RUVIA_CHECK(isFailure(result, HttpParseError::kInvalidTransferEncoding));
}

RUVIA_TEST(http1_parse_absolute_uri_host_mismatch_rejected) {
    // An absolute-form target whose authority disagrees with the Host header is
    // rejected to prevent routing ambiguity.
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "GET http://a.example/ HTTP/1.1\r\nHost: b.example\r\n\r\n");
    RUVIA_CHECK(isFailure(result, HttpParseError::kInvalidHost));
}

RUVIA_TEST(http1_parse_authority_uses_shared_uri_normalization) {
    Http1ServerRequestParser parser;
    // RFC 9110 section 4.2.3: an empty port is the scheme default, host is
    // case-insensitive, and percent-encoded unreserved octets normalize.
    const auto emptyPort = parser.parseMessage(
        "GET http://EXAMPLE.com:/path HTTP/1.1\r\n"
        "Host: example.com\r\n\r\n");
    RUVIA_CHECK(emptyPort.messageReady());

    const auto encodedHost = parser.parseMessage(
        "GET http://exa%6Dple.com/path HTTP/1.1\r\n"
        "Host: example.com:\r\n\r\n");
    RUVIA_CHECK(encodedHost.messageReady());

    const auto futureLiteral = parser.parseMessage(
        "GET http://[v1.future]/path HTTP/1.1\r\n"
        "Host: [V1.FUTURE]:\r\n\r\n");
    RUVIA_CHECK(futureLiteral.messageReady());
}

RUVIA_TEST(http1_parse_connect_requires_authority_form) {
    {
        Http1ServerRequestParser parser;
        const auto result = parser.parseMessage("CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n\r\n");
        RUVIA_CHECK(result.messageReady());
        RUVIA_CHECK_EQ(result.request.method(), std::string_view("CONNECT"));
        RUVIA_CHECK(result.request.knownMethod() == HttpKnownMethod::kConnect);
        RUVIA_CHECK_EQ(result.request.target(), std::string_view("example.com:443"));
        RUVIA_CHECK_EQ(result.request.path(), std::string_view("example.com:443"));
    }
    {
        Http1ServerRequestParser parser;
        const auto result = parser.parseMessage("CONNECT / HTTP/1.1\r\nHost: example.com\r\n\r\n");
        RUVIA_CHECK(isFailure(result, HttpParseError::kInvalidRequestTarget));
    }
    {
        Http1ServerRequestParser parser;
        const auto result = parser.parseMessage("CONNECT http://example.com:443 HTTP/1.1\r\nHost: example.com\r\n\r\n");
        RUVIA_CHECK(isFailure(result, HttpParseError::kInvalidRequestTarget));
    }
}

RUVIA_TEST(http1_parse_http10_without_host_allowed) {
    // HTTP/1.0 does not require a Host header.
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage("GET / HTTP/1.0\r\n\r\n");
    RUVIA_CHECK(result.messageReady());
    RUVIA_CHECK_EQ(result.request.method(), std::string_view("GET"));
    RUVIA_CHECK(result.request.knownMethod() == HttpKnownMethod::kGet);
    RUVIA_CHECK(
        result.request.protocolVersion() == HttpProtocolVersion::kHttp10);
}

RUVIA_TEST(http1_parse_http10_ignores_upgrade_field_semantics) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "GET / HTTP/1.0\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket/\r\n\r\n");

    RUVIA_CHECK(result.messageReady());
    RUVIA_CHECK(
        result.request.protocolVersion() == HttpProtocolVersion::kHttp10);
    RUVIA_CHECK_EQ(
        result.request.header("Upgrade"),
        std::optional<std::string_view>("websocket/"));
}

RUVIA_TEST(http1_parse_non_numeric_content_length_rejected) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n");
    RUVIA_CHECK(isFailure(result, HttpParseError::kInvalidContentLength));
}

RUVIA_TEST(http1_parse_too_many_headers_rejected) {
    std::string request = "GET / HTTP/1.1\r\nHost: x\r\n";
    for (int i = 0; i < 70; ++i) {  // exceeds kMaxHttpHeaderFields (64)
        request += "x-h-" + std::to_string(i) + ": v\r\n";
    }
    request += "\r\n";
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(request);
    RUVIA_CHECK(isFailure(result, HttpParseError::kTooManyHeaders));
}

RUVIA_TEST(http1_parse_chunk_size_overflow_rejected) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
        "10000000000000000\r\nx\r\n0\r\n\r\n");  // 2^64 chunk size overflows
    RUVIA_CHECK(isFailure(result, HttpParseError::kChunkSizeOverflow));
}

RUVIA_TEST(http1_parse_invalid_chunk_size_rejected) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\nx\r\n0\r\n\r\n");
    RUVIA_CHECK(isFailure(result, HttpParseError::kInvalidChunkSize));
}

RUVIA_TEST(http1_server_head_ready_is_distinct_from_message_ready) {
    Http1ServerRequestParser parser;
    Http1ServerRequestParseState head;
    constexpr std::string_view request =
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello";

    parser.parseHead(request, head);
    const auto* requestHead = head.headReady();
    RUVIA_CHECK(requestHead != nullptr);
    RUVIA_CHECK(head.messageReady() == nullptr);
    if (requestHead != nullptr) {
        RUVIA_CHECK_EQ(
            requestHead->headerBytes(),
            request.size() - std::string_view("hello").size());
    }
    RUVIA_CHECK_EQ(head.request.method(), std::string_view("POST"));
    RUVIA_CHECK(head.request.knownMethod() == HttpKnownMethod::kPost);

    const auto message = parser.parseMessage(request);
    const auto* messageReady = message.messageReady();
    RUVIA_CHECK(messageReady != nullptr);
    RUVIA_CHECK(message.headReady() == nullptr);
    if (messageReady != nullptr) {
        RUVIA_CHECK_EQ(messageReady->messageBytes(), request.size());
    }

    const auto bodyPending = parser.parseMessage(request.substr(0, request.size() - 1));
    const auto* needBody = bodyPending.needRequestBody();
    RUVIA_CHECK(needBody != nullptr);
    if (needBody != nullptr) {
        RUVIA_CHECK(needBody->requiredTotalBytes().has_value());
        if (needBody->requiredTotalBytes()) {
            RUVIA_CHECK_EQ(*needBody->requiredTotalBytes(), request.size());
        }
    }
}

RUVIA_TEST(http1_response_coding_folds_all_accept_encoding_field_lines) {
    Http1ServerRequestParser parser;
    const auto parsed = parser.parseMessage(
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "Accept-Encoding: identity;q=0, gzip;q=0.2\r\n"
        "Accept-Encoding: br;q=0.8\r\n\r\n");
    RUVIA_CHECK(parsed.messageReady() != nullptr);
    RUVIA_CHECK(
        parsed.responseCoding == ruvia::detail::HttpContentCoding::kBrotli);
}

RUVIA_TEST(http1_parse_header_block_too_large_rejected) {
    // A header section that reaches the byte cap without terminating is a DoS
    // guard (kMaxHttpHeaderBytes is 64 KiB).
    std::string request = "GET / HTTP/1.1\r\nX-Big: ";
    request += std::string(64 * 1024, 'a');  // pushes past the cap, no blank line
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(request);
    RUVIA_CHECK(isFailure(result, HttpParseError::kHeaderTooLarge));
}

RUVIA_TEST(http1_parse_invalid_request_target_rejected) {
    // An origin-form target must begin with '/'; a bare word is not a valid
    // request target for GET.
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage("GET foobar HTTP/1.1\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(isFailure(result, HttpParseError::kInvalidRequestTarget));
}
