#include "test_harness.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/http2/message/Http2RequestHeaders.h"

namespace {

using ruvia::HttpRequestExpectations;
using ruvia::HttpUnsupportedExpectationPolicy;
using ruvia::detail::http2AccumulateHeaderListBytes;
using ruvia::detail::Http2HeaderDecodeContext;
using ruvia::detail::http2OnDecodedInitialHeader;
using ruvia::detail::http2OnDecodedRequestTrailer;
using ruvia::detail::Http2StreamHeaderBlocks;
using ruvia::detail::Http2StreamRequestState;
using ruvia::detail::Http2StreamState;

std::pmr::memory_resource* res() noexcept {
    return std::pmr::new_delete_resource();
}

template <typename T>
concept HasValueSemanticRequestExpectations = requires(const T& value, const T&& temporary) {
    { value.requestExpectations() } -> std::same_as<HttpRequestExpectations>;
    { temporary.requestExpectations() } -> std::same_as<HttpRequestExpectations>;
};

static_assert(HasValueSemanticRequestExpectations<Http2StreamState>);

template <typename T>
concept ExposesRvalueHttp2StreamRequestStateStorage =
    requires(T&& state) { std::move(state).responseStatus(); };

template <typename T>
concept ExposesRvalueHttp2StreamHeaderBlocksStorage =
    requires(T&& blocks) { std::move(blocks).request(); } ||
    requires(const T&& blocks) { std::move(blocks).request(); } ||
    requires(T&& blocks) { std::move(blocks).response(); } ||
    requires(const T&& blocks) { std::move(blocks).response(); };

template <typename T>
concept ExposesRvalueHttp2StreamStateStorage =
    requires(T&& stream) { std::move(stream).receiveWindowCredit(); } ||
    requires(T&& stream) { std::move(stream).remoteHeaderBlock(); } ||
    requires(const T&& stream) { std::move(stream).remoteHeaderBlock(); } ||
    requires(T&& stream) { std::move(stream).localHeaderBlock(); } ||
    requires(const T&& stream) { std::move(stream).localHeaderBlock(); } ||
    requires(T&& stream) { std::move(stream).remoteContent(); } ||
    requires(T&& stream) { std::move(stream).localContent(); } ||
    requires(T&& stream) { std::move(stream).localSend(); } ||
    requires(T&& stream) { std::move(stream).remoteReceive(); } ||
    requires(T&& stream) { std::move(stream).requestMethod(); } ||
    requires(T&& stream) { std::move(stream).requestAuthority(); } ||
    requires(T&& stream) { std::move(stream).requestPath(); } ||
    requires(T&& stream) { std::move(stream).requestProtocol(); } ||
    requires(T&& stream) { std::move(stream).requestCookie(); } ||
    requires(T&& stream) { std::move(stream).remoteHeaderAt(std::size_t{}); } ||
    requires(T&& stream) { std::move(stream).requestScheme(); } ||
    requires(T&& stream) { std::move(stream).tunnel(); } ||
    requires(T&& stream) { std::move(stream).responseStatus(); };

static_assert(!ExposesRvalueHttp2StreamRequestStateStorage<Http2StreamRequestState>);
static_assert(!ExposesRvalueHttp2StreamHeaderBlocksStorage<Http2StreamHeaderBlocks>);
static_assert(!ExposesRvalueHttp2StreamStateStorage<Http2StreamState>);

}  // namespace

RUVIA_TEST(h2_response_status_is_optional_and_single_assignment) {
    Http2StreamState stream(1, res());
    RUVIA_CHECK(stream.responseStatus() == nullptr);
    RUVIA_CHECK(stream.setResponseStatus(ruvia::http_status::kOk));
    const auto* status = stream.responseStatus();
    RUVIA_CHECK(status != nullptr);
    if (status != nullptr) {
        RUVIA_CHECK_EQ(*status, ruvia::http_status::kOk);
    }
    RUVIA_CHECK(!stream.setResponseStatus(ruvia::http_status::kNoContent));
    status = stream.responseStatus();
    RUVIA_CHECK(status != nullptr);
    if (status != nullptr) {
        RUVIA_CHECK_EQ(*status, ruvia::http_status::kOk);
    }
}

RUVIA_TEST(h2_headers_valid_pseudo_headers_stored) {
    Http2StreamState stream(1, res());
    Http2HeaderDecodeContext ctx{stream};
    RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, ":method", "GET"));
    RUVIA_CHECK(stream.hasMethod());
    RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, ":scheme", "https"));
    RUVIA_CHECK(stream.hasScheme());
    RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, ":authority", "example.com"));
    RUVIA_CHECK_EQ(stream.requestAuthority(), std::string_view("example.com"));
    RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, ":path", "/index"));
    RUVIA_CHECK_EQ(stream.requestPath(), std::string_view("/index"));
}

RUVIA_TEST(h2_headers_duplicate_pseudo_header_rejected) {
    Http2StreamState stream(1, res());
    Http2HeaderDecodeContext ctx{stream};
    RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, ":method", "GET"));
    RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, ":method", "POST"));  // duplicate
    RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, ":scheme", "https"));
    RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, ":scheme", "http"));  // duplicate
}

RUVIA_TEST(h2_headers_empty_and_unknown_pseudo_rejected) {
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, ":method", ""));  // empty method
    }
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, ":method", "BAD METHOD"));
    }
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, ":scheme", ""));  // empty scheme
    }
    for (const auto malformed : {"1ftp", "bad scheme", "ftp:"}) {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, ":scheme", malformed));
    }
    Http2StreamState stream(1, res());
    Http2HeaderDecodeContext ctx{stream};
    RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, ":protocol", ""));  // empty protocol
    RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, ":unknown", "x"));  // unknown pseudo-header
    RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, "", "x"));          // empty name
}

RUVIA_TEST(h2_headers_empty_path_is_present_and_deferred_to_scheme) {
    Http2StreamState stream(1, res());
    Http2HeaderDecodeContext ctx{stream};
    RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, ":path", ""));
    RUVIA_CHECK(stream.hasPath());
    RUVIA_CHECK(stream.requestPath().empty());
}

RUVIA_TEST(h2_headers_empty_generic_authority_is_deferred_to_scheme) {
    Http2StreamState stream(1, res());
    Http2HeaderDecodeContext ctx{stream};
    RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, ":authority", ""));
    RUVIA_CHECK(stream.hasAuthority());
    RUVIA_CHECK(stream.requestAuthority().empty());
}

RUVIA_TEST(h2_headers_extension_method_is_valid_and_preserved) {
    Http2StreamState stream(1, res());
    Http2HeaderDecodeContext ctx{stream};
    RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, ":method", "PROPFIND"));
    RUVIA_CHECK_EQ(stream.requestMethod(), std::string_view("PROPFIND"));
    RUVIA_CHECK(stream.requestKnownMethod() == ruvia::HttpKnownMethod::kUnknown);
}

RUVIA_TEST(h2_headers_authority_and_host_are_validated) {
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, ":authority", "bad host"));
    }
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, ":authority", "example.com:"));
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, "host", "EXA%6dPLE.com"));
    }
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, ":authority", "[v1.future]:"));
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, "host", "[V1.FUTURE]"));
    }
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, "host", "bad host"));
    }
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, ":authority", "example.com"));
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, "host", "EXAMPLE.com"));
    }
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, ":authority", "example.com"));
        RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, "host", "other.example"));
    }
}

RUVIA_TEST(h2_headers_authority_host_match_uses_scheme_default_port) {
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, ":scheme", "https"));
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, ":authority", "example.com:443"));
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, "host", "example.com"));
    }
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, ":scheme", "http"));
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, ":authority", "example.com:80"));
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, "host", "example.com"));
    }
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, ":scheme", "https"));
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, ":authority", "example.com:80"));
        RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, "host", "example.com"));
    }
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, ":scheme", "ftp"));
        RUVIA_CHECK_EQ(stream.requestScheme(), std::string_view("ftp"));
        RUVIA_CHECK_EQ(stream.schemeDefaultPort(), std::uint16_t{0});
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, ":authority", "example.com"));
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, "host", "example.com:"));
    }
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, ":scheme", "ftp"));
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, ":authority", "example.com:21"));
        RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, "host", "example.com"));
    }
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, ":scheme", "ftp"));
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, ":authority", "example.com:21"));
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, "host", "example.com:21"));
    }
}

RUVIA_TEST(h2_headers_path_rejects_malformed_origin_target) {
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        // Method can follow :path, so field-level decoding accepts the
        // asterisk syntax and final head validation enforces OPTIONS-only.
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, ":path", "*"));
    }
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, ":path", "relative"));
    }
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, ":path", "/bad#fragment"));
    }
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, ":path", "/bad\\path"));
    }
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, ":path", "/bad%zz"));
    }
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, ":path", "/bad%"));
    }
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, ":path", "/ok%2F?q=%7B%7D"));
        RUVIA_CHECK_EQ(stream.requestPath(), std::string_view("/ok%2F?q=%7B%7D"));
    }
}

RUVIA_TEST(h2_headers_pseudo_after_regular_rejected) {
    // Pseudo-headers must precede all regular headers (RFC 7540 8.1.2.1).
    Http2StreamState stream(1, res());
    Http2HeaderDecodeContext ctx{stream};
    RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, "accept", "text/html"));
    RUVIA_CHECK(stream.regularHeaderSeen());
    RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, ":method", "GET"));
}

RUVIA_TEST(h2_headers_invalid_regular_header_rejected) {
    Http2StreamState stream(1, res());
    Http2HeaderDecodeContext ctx{stream};
    // Uppercase name, connection-specific headers, and field values with
    // leading/trailing SP/HTAB are malformed in HTTP/2.
    RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, "Accept", "text/html"));
    RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, "connection", "close"));
    RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, "transfer-encoding", "chunked"));
    RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, "x-test", " value"));
    RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, "x-test", "value\t"));
}

RUVIA_TEST(h2_headers_connection_specific_and_te_rules) {
    // RFC 7540 8.1.2.2: every connection-specific header is malformed in HTTP/2
    // (connection and transfer-encoding are checked above; pin the remaining three
    // so none can be dropped from the ban without a failing test).
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, "keep-alive", "timeout=5"));
    }
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, "proxy-connection", "keep-alive"));
    }
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, "upgrade", "websocket"));
    }
    // TE is the single exception: permitted only with the exact value "trailers".
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, "te", "gzip"));
    }
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, "te", "trailers"));  // the allowed form
    }
}

RUVIA_TEST(h2_headers_duplicate_host_rejected) {
    Http2StreamState stream(1, res());
    Http2HeaderDecodeContext ctx{stream};
    RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, "host", "a.example"));
    RUVIA_CHECK(stream.hasHost());
    RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, "host", "b.example"));  // duplicate host
}

RUVIA_TEST(h2_headers_duplicate_singleton_regular_headers_rejected) {
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, "content-type", "text/plain"));
        RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, "content-type", "application/json"));
    }
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, "range", "bytes=0-99"));
        RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, "range", "bytes=200-299"));
    }
}

RUVIA_TEST(h2_headers_repeated_etag_list_fields_accepted) {
    Http2StreamState stream(1, res());
    Http2HeaderDecodeContext ctx{stream};
    RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, "if-none-match", R"("old")"));
    RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, "if-none-match", R"("new")"));
    RUVIA_CHECK_EQ(stream.remoteHeaderCount(), std::size_t{2});
}

RUVIA_TEST(h2_headers_duplicate_auth_and_cors_singletons_rejected) {
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, "authorization", "Bearer first"));
        RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, "authorization", "Bearer second"));
    }
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, "origin", "https://a.example"));
        RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, "origin", "https://b.example"));
    }
    {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, "access-control-request-method", "GET"));
        RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, "access-control-request-method", "POST"));
    }
}

RUVIA_TEST(h2_headers_duplicate_websocket_identity_and_user_agent_rejected) {
    struct Case final {
        std::string_view name;
        std::string_view first;
        std::string_view second;
    };
    const Case cases[] = {
        {"sec-websocket-key", "first", "second"},
        {"sec-websocket-version", "13", "12"},
        {"user-agent", "first/1", "second/2"},
    };
    for (const auto& test : cases) {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        RUVIA_CHECK(http2OnDecodedInitialHeader(ctx, test.name, test.first));
        RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, test.name, test.second));
    }
}

RUVIA_TEST(h2_headers_enforce_cors_request_field_grammar) {
    const auto accepts = [](std::string_view name, std::string_view value) {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        return http2OnDecodedInitialHeader(ctx, name, value);
    };

    RUVIA_CHECK(accepts("origin", "null"));
    RUVIA_CHECK(accepts("origin", "https://first.example https://second.example"));
    RUVIA_CHECK(accepts("access-control-request-method", "PATCH"));
    RUVIA_CHECK(accepts("access-control-request-headers", ", x-one,, x-two,"));

    RUVIA_CHECK(!accepts("origin", "*"));
    RUVIA_CHECK(!accepts("origin", "https://app.example/"));
    RUVIA_CHECK(!accepts("origin", "https://APP.example"));
    RUVIA_CHECK(!accepts("origin", "https://app.example:443"));
    RUVIA_CHECK(!accepts("origin", "https://app.example:65536"));
    RUVIA_CHECK(!accepts("access-control-request-method", "POST, DELETE"));
    RUVIA_CHECK(!accepts("access-control-request-method", "POST /admin"));
    RUVIA_CHECK(!accepts("access-control-request-headers", ""));
    RUVIA_CHECK(!accepts("access-control-request-headers", ", ,"));
    RUVIA_CHECK(!accepts("access-control-request-headers", "x-good, x bad"));
}

RUVIA_TEST(h2_headers_content_length_and_cookie) {
    Http2StreamState stream(1, res());
    Http2HeaderDecodeContext ctx{stream};
    // A non-numeric Content-Length is rejected.
    RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, "content-length", "abc"));

    Http2StreamState stream2(1, res());
    Http2HeaderDecodeContext ctx2{stream2};
    RUVIA_CHECK(http2OnDecodedInitialHeader(ctx2, "content-length", "42"));
    RUVIA_CHECK(http2OnDecodedInitialHeader(ctx2, "content-length", "42"));
    Http2StreamState listLength(3, res());
    Http2HeaderDecodeContext listLengthCtx{listLength};
    RUVIA_CHECK(http2OnDecodedInitialHeader(listLengthCtx, "content-length", "42, 42"));

    Http2StreamState conflictingLength(5, res());
    Http2HeaderDecodeContext conflictingLengthCtx{conflictingLength};
    RUVIA_CHECK(!http2OnDecodedInitialHeader(conflictingLengthCtx, "content-length", "42, 43"));

    Http2StreamState repeatedConflict(7, res());
    Http2HeaderDecodeContext repeatedConflictCtx{repeatedConflict};
    RUVIA_CHECK(http2OnDecodedInitialHeader(repeatedConflictCtx, "content-length", "42"));
    RUVIA_CHECK(!http2OnDecodedInitialHeader(repeatedConflictCtx, "content-length", "43"));

    // Split Cookie headers accumulate on the stream.
    RUVIA_CHECK(http2OnDecodedInitialHeader(ctx2, "cookie", "a=1"));
    RUVIA_CHECK(http2OnDecodedInitialHeader(ctx2, "cookie", "b=2"));
    RUVIA_CHECK(stream2.hasCookie());
    RUVIA_CHECK_EQ(stream2.requestCookie(), std::string_view("a=1; b=2"));
}

RUVIA_TEST(h2_headers_field_limit_counts_coalesced_cookie_lines) {
    Http2StreamState stream(1, res());
    Http2HeaderDecodeContext context{stream};
    for (std::size_t i = 0; i < ruvia::kMaxHttpHeaderFields; ++i) {
        RUVIA_CHECK(http2OnDecodedInitialHeader(context, "cookie", "a=1"));
    }
    RUVIA_CHECK(!http2OnDecodedInitialHeader(context, "cookie", "a=1"));
}

RUVIA_TEST(h2_headers_expect_is_an_extensible_repeated_list) {
    Http2StreamState supported(1, res());
    Http2HeaderDecodeContext supportedContext{supported};
    RUVIA_CHECK(http2OnDecodedInitialHeader(supportedContext, "expect", ", 100-continue,"));
    RUVIA_CHECK(http2OnDecodedInitialHeader(supportedContext, "expect", "100-Continue"));
    RUVIA_CHECK(supported.finalizeRemoteContentHead());
    const auto supportedPlan = supported.expectationPlan(HttpUnsupportedExpectationPolicy::kReject);
    RUVIA_CHECK(supportedPlan.sendContinue() != nullptr);

    RUVIA_CHECK(supported.finishRemoteContent());
    const auto completedPlan = supported.expectationPlan(HttpUnsupportedExpectationPolicy::kReject);
    RUVIA_CHECK(completedPlan.noAction() != nullptr);

    Http2StreamState zeroLength(2, res());
    Http2HeaderDecodeContext zeroLengthContext{zeroLength};
    RUVIA_CHECK(http2OnDecodedInitialHeader(zeroLengthContext, "expect", "100-continue"));
    RUVIA_CHECK(http2OnDecodedInitialHeader(zeroLengthContext, "content-length", "0"));
    RUVIA_CHECK(zeroLength.finalizeRemoteContentHead());
    const auto zeroLengthPlan =
        zeroLength.expectationPlan(HttpUnsupportedExpectationPolicy::kReject);
    RUVIA_CHECK(zeroLengthPlan.noAction() != nullptr);
    RUVIA_CHECK(zeroLengthPlan.sendContinue() == nullptr);

    Http2StreamState completedLength(4, res());
    Http2HeaderDecodeContext completedLengthContext{completedLength};
    RUVIA_CHECK(http2OnDecodedInitialHeader(completedLengthContext, "expect", "100-continue"));
    RUVIA_CHECK(http2OnDecodedInitialHeader(completedLengthContext, "content-length", "1"));
    RUVIA_CHECK(completedLength.finalizeRemoteContentHead());
    const auto pendingLengthPlan =
        completedLength.expectationPlan(HttpUnsupportedExpectationPolicy::kReject);
    RUVIA_CHECK(pendingLengthPlan.sendContinue() != nullptr);
    RUVIA_CHECK(completedLength.accountRemoteContent(1) ==
                ruvia::detail::Http2RemoteContentAccountingResult::kAccepted);
    const auto completedLengthPlan =
        completedLength.expectationPlan(HttpUnsupportedExpectationPolicy::kReject);
    RUVIA_CHECK(completedLengthPlan.noAction() != nullptr);

    Http2StreamState extension(3, res());
    Http2HeaderDecodeContext extensionContext{extension};
    RUVIA_CHECK(
        http2OnDecodedInitialHeader(extensionContext, "expect", "100-continue, custom-feature"));
    RUVIA_CHECK(extension.finalizeRemoteContentHead());
    const auto extensionPlan = extension.expectationPlan(HttpUnsupportedExpectationPolicy::kReject);
    RUVIA_CHECK(extensionPlan.rejection() != nullptr);
    RUVIA_CHECK(extension.requestExpectations().hasContinue());
    RUVIA_CHECK(extension.requestExpectations().hasUnsupported());

    Http2StreamState malformed(5, res());
    Http2HeaderDecodeContext malformedContext{malformed};
    RUVIA_CHECK(!http2OnDecodedInitialHeader(malformedContext, "expect", "bad value"));
    RUVIA_CHECK(!malformed.requestExpectations().hasContinue());
    RUVIA_CHECK(!malformed.requestExpectations().hasUnsupported());
}

RUVIA_TEST(h2_headers_trailer_rejects_pseudo_and_invalid) {
    Http2StreamState stream(1, res());
    Http2HeaderDecodeContext ctx{stream};
    RUVIA_CHECK(http2OnDecodedRequestTrailer(ctx, "x-trace-id", "abc"));
    RUVIA_CHECK(
        !http2OnDecodedRequestTrailer(ctx, ":method", "GET"));  // no pseudo-headers in trailers
    RUVIA_CHECK(
        !http2OnDecodedRequestTrailer(ctx, "connection", "close"));  // forbidden framing field
    RUVIA_CHECK(
        !http2OnDecodedRequestTrailer(ctx, "host", "example.com"));  // routing is header-only
    RUVIA_CHECK(
        !http2OnDecodedRequestTrailer(ctx, "content-length", "0"));  // framing is header-only
    RUVIA_CHECK(
        !http2OnDecodedRequestTrailer(ctx, "te", "trailers"));  // connection option is header-only
    RUVIA_CHECK(!http2OnDecodedRequestTrailer(ctx, "trailer", "x-checksum"));
    RUVIA_CHECK(!http2OnDecodedRequestTrailer(ctx, "content-type", "text/plain"));
    RUVIA_CHECK(!http2OnDecodedRequestTrailer(ctx, "origin", "https://app.example"));
    RUVIA_CHECK(!http2OnDecodedRequestTrailer(ctx, "access-control-request-method", "POST"));
    RUVIA_CHECK(!http2OnDecodedRequestTrailer(ctx, "access-control-request-headers", "x-one"));
}

RUVIA_TEST(h2_headers_validate_initial_trailer_field_names) {
    Http2StreamState valid(1, res());
    Http2HeaderDecodeContext validContext{valid};
    RUVIA_CHECK(http2OnDecodedInitialHeader(validContext, "trailer", "x-checksum, x-signature"));

    Http2StreamState empty(3, res());
    Http2HeaderDecodeContext emptyContext{empty};
    RUVIA_CHECK(http2OnDecodedInitialHeader(emptyContext, "trailer", ","));

    Http2StreamState malformed(5, res());
    Http2HeaderDecodeContext malformedContext{malformed};
    RUVIA_CHECK(!http2OnDecodedInitialHeader(malformedContext, "trailer", "x-checksum, bad field"));

    Http2StreamState forbidden(7, res());
    Http2HeaderDecodeContext forbiddenContext{forbidden};
    RUVIA_CHECK(!http2OnDecodedInitialHeader(forbiddenContext, "trailer", "Content-Length"));
}

RUVIA_TEST(h2_headers_trailer_enforces_field_count_without_storing_fields) {
    Http2StreamState stream(1, res());
    Http2HeaderDecodeContext context{stream};
    for (std::size_t i = 0; i < ruvia::kMaxHttpHeaderFields; ++i) {
        RUVIA_CHECK(http2OnDecodedRequestTrailer(context, "x-trace", "value"));
    }
    RUVIA_CHECK(!http2OnDecodedRequestTrailer(context, "x-trace", "value"));
}

RUVIA_TEST(h2_headers_list_byte_limit) {
    Http2StreamState stream(1, res());
    Http2HeaderDecodeContext ctx{stream};
    RUVIA_CHECK(http2AccumulateHeaderListBytes(ctx, "accept", "text/html"));
    RUVIA_CHECK(ctx.decodedHeaderListSize.bytes() > 0);
    // A single field larger than the whole header budget is rejected.
    const std::string big(64 * 1024, 'x');
    RUVIA_CHECK(!http2AccumulateHeaderListBytes(ctx, "name", big));
}

RUVIA_TEST(h2_headers_list_byte_limit_accumulates_across_entries) {
    // The real header-flood DoS vector: many individually-legal headers that
    // together exceed the 64 KiB list budget (each entry also costs a 32-byte
    // overhead, RFC 9113 6.5.2). The accumulator must reject once the running total
    // would exceed the budget -- not merely reject a single oversized field.
    Http2StreamState stream(1, res());
    Http2HeaderDecodeContext ctx{stream};
    const std::string value(1000, 'v');  // ~1037 bytes per entry incl. name + overhead
    bool rejected = false;
    for (int i = 0; i < 200 && !rejected; ++i) {
        rejected = !http2AccumulateHeaderListBytes(ctx, "x-pad", value);
    }
    RUVIA_CHECK(rejected);                                        // the running total is bounded
    RUVIA_CHECK(ctx.decodedHeaderListSize.bytes() <= 64 * 1024);  // never exceeds the budget
}
