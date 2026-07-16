#include "test_harness.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/detail/http2/Http2RequestHeaders.h"

namespace {

using ruvia::detail::Http2HeaderDecodeContext;
using ruvia::detail::Http2StreamState;
using ruvia::detail::HttpRequestExpectations;
using ruvia::detail::HttpUnsupportedExpectationPolicy;
using ruvia::detail::http2AccumulateHeaderListBytes;
using ruvia::detail::http2OnDecodedInitialHeader;
using ruvia::detail::http2OnDecodedTrailer;

std::pmr::memory_resource* res() noexcept {
    return std::pmr::new_delete_resource();
}

template <typename T>
concept HasValueSemanticRequestExpectations = requires(const T& value, const T&& temporary) {
    { value.requestExpectations() } -> std::same_as<HttpRequestExpectations>;
    { temporary.requestExpectations() } -> std::same_as<HttpRequestExpectations>;
};

static_assert(HasValueSemanticRequestExpectations<Http2StreamState>);

}  // namespace

RUVIA_TEST(h2_response_status_is_optional_and_single_assignment) {
    Http2StreamState stream(1, res());
    RUVIA_CHECK(stream.responseStatus() == nullptr);
    RUVIA_CHECK(stream.setResponseStatus(200));
    const auto* status = stream.responseStatus();
    RUVIA_CHECK(status != nullptr);
    if (status != nullptr) {
        RUVIA_CHECK_EQ(*status, std::uint16_t{200});
    }
    RUVIA_CHECK(!stream.setResponseStatus(204));
    status = stream.responseStatus();
    RUVIA_CHECK(status != nullptr);
    if (status != nullptr) {
        RUVIA_CHECK_EQ(*status, std::uint16_t{200});
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
    RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, ":path", ""));      // empty path
    RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, ":authority", "")); // empty authority
    RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, ":protocol", ""));  // empty protocol
    RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, ":unknown", "x"));  // unknown pseudo-header
    RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, "", "x"));          // empty name
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
    // Uppercase name and connection-specific headers are malformed in HTTP/2.
    RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, "Accept", "text/html"));
    RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, "connection", "close"));
    RUVIA_CHECK(!http2OnDecodedInitialHeader(ctx, "transfer-encoding", "chunked"));
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
    RUVIA_CHECK_EQ(stream.requestHeaderCount(), std::size_t{2});
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

RUVIA_TEST(h2_headers_enforce_cors_request_field_grammar) {
    const auto accepts = [](std::string_view name, std::string_view value) {
        Http2StreamState stream(1, res());
        Http2HeaderDecodeContext ctx{stream};
        return http2OnDecodedInitialHeader(ctx, name, value);
    };

    RUVIA_CHECK(accepts("origin", "null"));
    RUVIA_CHECK(accepts(
        "origin",
        "https://first.example https://second.example"));
    RUVIA_CHECK(accepts("access-control-request-method", "PATCH"));
    RUVIA_CHECK(accepts(
        "access-control-request-headers",
        ", x-one,, x-two,"));

    RUVIA_CHECK(!accepts("origin", "*"));
    RUVIA_CHECK(!accepts("origin", "https://app.example/"));
    RUVIA_CHECK(!accepts("origin", "https://APP.example"));
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
    // Split Cookie headers accumulate on the stream.
    RUVIA_CHECK(http2OnDecodedInitialHeader(ctx2, "cookie", "a=1"));
    RUVIA_CHECK(http2OnDecodedInitialHeader(ctx2, "cookie", "b=2"));
    RUVIA_CHECK(stream2.hasCookie());
    RUVIA_CHECK_EQ(stream2.requestCookie(), std::string_view("a=1; b=2"));
}

RUVIA_TEST(h2_headers_expect_is_an_extensible_repeated_list) {
    Http2StreamState supported(1, res());
    Http2HeaderDecodeContext supportedContext{supported};
    RUVIA_CHECK(http2OnDecodedInitialHeader(
        supportedContext, "expect", ", 100-continue,"));
    RUVIA_CHECK(http2OnDecodedInitialHeader(
        supportedContext, "expect", "100-Continue"));
    RUVIA_CHECK(supported.finalizeRemoteContentHead());
    const auto supportedPlan = supported.expectationPlan(
        HttpUnsupportedExpectationPolicy::kReject);
    RUVIA_CHECK(supportedPlan.send100Continue() != nullptr);

    RUVIA_CHECK(supported.finishRemoteContent());
    const auto completedPlan = supported.expectationPlan(
        HttpUnsupportedExpectationPolicy::kReject);
    RUVIA_CHECK(completedPlan.noAction() != nullptr);

    Http2StreamState extension(3, res());
    Http2HeaderDecodeContext extensionContext{extension};
    RUVIA_CHECK(http2OnDecodedInitialHeader(
        extensionContext, "expect", "100-continue, custom-feature"));
    RUVIA_CHECK(extension.finalizeRemoteContentHead());
    const auto extensionPlan = extension.expectationPlan(
        HttpUnsupportedExpectationPolicy::kReject);
    RUVIA_CHECK(extensionPlan.rejection() != nullptr);
    RUVIA_CHECK(extension.requestExpectations().has100Continue());
    RUVIA_CHECK(extension.requestExpectations().hasUnsupported());
}

RUVIA_TEST(h2_headers_trailer_rejects_pseudo_and_invalid) {
    Http2StreamState stream(1, res());
    Http2HeaderDecodeContext ctx{stream};
    RUVIA_CHECK(http2OnDecodedTrailer(ctx, "x-trace-id", "abc"));
    RUVIA_CHECK(!http2OnDecodedTrailer(ctx, ":method", "GET"));      // no pseudo-headers in trailers
    RUVIA_CHECK(!http2OnDecodedTrailer(ctx, "connection", "close"));  // forbidden framing field
    RUVIA_CHECK(!http2OnDecodedTrailer(ctx, "host", "example.com"));  // routing is header-only
    RUVIA_CHECK(!http2OnDecodedTrailer(ctx, "content-length", "0"));  // framing is header-only
    RUVIA_CHECK(!http2OnDecodedTrailer(ctx, "te", "trailers"));       // connection option is header-only
    RUVIA_CHECK(!http2OnDecodedTrailer(ctx, "trailer", "x-checksum"));
    RUVIA_CHECK(!http2OnDecodedTrailer(ctx, "content-type", "text/plain"));
    RUVIA_CHECK(!http2OnDecodedTrailer(ctx, "origin", "https://app.example"));
    RUVIA_CHECK(!http2OnDecodedTrailer(
        ctx,
        "access-control-request-method",
        "POST"));
    RUVIA_CHECK(!http2OnDecodedTrailer(
        ctx,
        "access-control-request-headers",
        "x-one"));
}

RUVIA_TEST(h2_headers_list_byte_limit) {
    Http2StreamState stream(1, res());
    Http2HeaderDecodeContext ctx{stream};
    RUVIA_CHECK(http2AccumulateHeaderListBytes(ctx, "accept", "text/html"));
    RUVIA_CHECK(ctx.decodedHeaderListBytes > 0);
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
    RUVIA_CHECK(rejected);                                    // the running total is bounded
    RUVIA_CHECK(ctx.decodedHeaderListBytes <= 64 * 1024);    // never exceeds the budget
}
