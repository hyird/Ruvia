#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "http2/http2_request_headers.h"
#include "test_harness.h"

namespace {

using ruvia::http_request_expectations;
using ruvia::http_unsupported_expectation_policy;
using ruvia::detail::http2_accumulate_header_list_bytes;
using ruvia::detail::http2_header_decode_context;
using ruvia::detail::http2_on_decoded_initial_header;
using ruvia::detail::http2_on_decoded_request_trailer;
using ruvia::detail::http2_stream_header_blocks;
using ruvia::detail::http2_stream_request_state;
using ruvia::detail::http2_stream_state;

std::pmr::memory_resource* res() noexcept {
    return std::pmr::new_delete_resource();
}

}  // namespace

RUVIA_TEST(h2_response_status_is_optional_and_single_assignment) {
    http2_stream_state stream(1, res());
    RUVIA_CHECK(stream.response_status() == nullptr);
    RUVIA_CHECK(stream.set_response_status(ruvia::http_status::ok));
    const auto* status = stream.response_status();
    RUVIA_CHECK(status != nullptr);
    if (status != nullptr) {
        RUVIA_CHECK_EQ(*status, ruvia::http_status::ok);
    }
    RUVIA_CHECK(!stream.set_response_status(ruvia::http_status::no_content));
    status = stream.response_status();
    RUVIA_CHECK(status != nullptr);
    if (status != nullptr) {
        RUVIA_CHECK_EQ(*status, ruvia::http_status::ok);
    }
}

RUVIA_TEST(h2_headers_valid_pseudo_headers_stored) {
    http2_stream_state stream(1, res());
    http2_header_decode_context ctx{stream};
    RUVIA_CHECK(http2_on_decoded_initial_header(ctx, ":method", "GET"));
    RUVIA_CHECK(stream.has_method());
    RUVIA_CHECK(http2_on_decoded_initial_header(ctx, ":scheme", "https"));
    RUVIA_CHECK(stream.has_scheme());
    RUVIA_CHECK(http2_on_decoded_initial_header(ctx, ":authority", "example.com"));
    RUVIA_CHECK_EQ(stream.request_authority(), std::string_view("example.com"));
    RUVIA_CHECK(http2_on_decoded_initial_header(ctx, ":path", "/index"));
    RUVIA_CHECK_EQ(stream.request_path(), std::string_view("/index"));
}

RUVIA_TEST(h2_headers_duplicate_pseudo_header_rejected) {
    http2_stream_state stream(1, res());
    http2_header_decode_context ctx{stream};
    RUVIA_CHECK(http2_on_decoded_initial_header(ctx, ":method", "GET"));
    RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, ":method", "POST"));  // duplicate
    RUVIA_CHECK(http2_on_decoded_initial_header(ctx, ":scheme", "https"));
    RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, ":scheme", "http"));  // duplicate
}

RUVIA_TEST(h2_headers_empty_and_unknown_pseudo_rejected) {
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, ":method", ""));  // empty method
    }
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, ":method", "BAD METHOD"));
    }
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, ":scheme", ""));  // empty scheme
    }
    for (const auto malformed : {"1ftp", "bad scheme", "ftp:"}) {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, ":scheme", malformed));
    }
    http2_stream_state stream(1, res());
    http2_header_decode_context ctx{stream};
    RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, ":protocol", ""));  // empty protocol
    RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, ":unknown", "x"));  // unknown pseudo-header
    RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, "", "x"));          // empty name
}

RUVIA_TEST(h2_headers_empty_path_is_present_and_deferred_to_scheme) {
    http2_stream_state stream(1, res());
    http2_header_decode_context ctx{stream};
    RUVIA_CHECK(http2_on_decoded_initial_header(ctx, ":path", ""));
    RUVIA_CHECK(stream.has_path());
    RUVIA_CHECK(stream.request_path().empty());
}

RUVIA_TEST(h2_headers_empty_generic_authority_is_deferred_to_scheme) {
    http2_stream_state stream(1, res());
    http2_header_decode_context ctx{stream};
    RUVIA_CHECK(http2_on_decoded_initial_header(ctx, ":authority", ""));
    RUVIA_CHECK(stream.has_authority());
    RUVIA_CHECK(stream.request_authority().empty());
}

RUVIA_TEST(h2_headers_extension_method_is_valid_and_preserved) {
    http2_stream_state stream(1, res());
    http2_header_decode_context ctx{stream};
    RUVIA_CHECK(http2_on_decoded_initial_header(ctx, ":method", "PROPFIND"));
    RUVIA_CHECK_EQ(stream.request_method(), std::string_view("PROPFIND"));
    RUVIA_CHECK(stream.request_known_method() == ruvia::http_known_method::unknown);
}

RUVIA_TEST(h2_headers_authority_and_host_are_validated) {
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, ":authority", "bad host"));
    }
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, ":authority", "example.com:"));
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, "host", "EXA%6dPLE.com"));
    }
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, ":authority", "[v1.future]:"));
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, "host", "[V1.FUTURE]"));
    }
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, "host", "bad host"));
    }
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, ":authority", "example.com"));
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, "host", "EXAMPLE.com"));
    }
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, ":authority", "example.com"));
        RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, "host", "other.example"));
    }
}

RUVIA_TEST(h2_headers_authority_host_match_uses_scheme_default_port) {
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, ":scheme", "https"));
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, ":authority", "example.com:443"));
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, "host", "example.com"));
    }
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, ":scheme", "http"));
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, ":authority", "example.com:80"));
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, "host", "example.com"));
    }
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, ":scheme", "https"));
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, ":authority", "example.com:80"));
        RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, "host", "example.com"));
    }
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, ":scheme", "ftp"));
        RUVIA_CHECK_EQ(stream.request_scheme(), std::string_view("ftp"));
        RUVIA_CHECK_EQ(stream.scheme_default_port(), std::uint16_t{0});
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, ":authority", "example.com"));
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, "host", "example.com:"));
    }
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, ":scheme", "ftp"));
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, ":authority", "example.com:21"));
        RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, "host", "example.com"));
    }
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, ":scheme", "ftp"));
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, ":authority", "example.com:21"));
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, "host", "example.com:21"));
    }
}

RUVIA_TEST(h2_headers_path_rejects_malformed_origin_target) {
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        // Method can follow :path, so field-level decoding accepts the
        // asterisk syntax and final head validation enforces OPTIONS-only.
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, ":path", "*"));
    }
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, ":path", "relative"));
    }
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, ":path", "/bad#fragment"));
    }
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, ":path", "/bad\\path"));
    }
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, ":path", "/bad%zz"));
    }
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, ":path", "/bad%"));
    }
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, ":path", "/ok%2F?q=%7B%7D"));
        RUVIA_CHECK_EQ(stream.request_path(), std::string_view("/ok%2F?q=%7B%7D"));
    }
}

RUVIA_TEST(h2_headers_pseudo_after_regular_rejected) {
    // Pseudo-headers must precede all regular headers (RFC 7540 8.1.2.1).
    http2_stream_state stream(1, res());
    http2_header_decode_context ctx{stream};
    RUVIA_CHECK(http2_on_decoded_initial_header(ctx, "accept", "text/html"));
    RUVIA_CHECK(stream.regular_header_seen());
    RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, ":method", "GET"));
}

RUVIA_TEST(h2_headers_invalid_regular_header_rejected) {
    http2_stream_state stream(1, res());
    http2_header_decode_context ctx{stream};
    // Uppercase name, connection-specific headers, and field values with
    // leading/trailing SP/HTAB are malformed in HTTP/2.
    RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, "Accept", "text/html"));
    RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, "connection", "close"));
    RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, "transfer-encoding", "chunked"));
    RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, "x-test", " value"));
    RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, "x-test", "value\t"));
}

RUVIA_TEST(h2_headers_connection_specific_and_te_rules) {
    // RFC 7540 8.1.2.2: every connection-specific header is malformed in HTTP/2
    // (connection and transfer-encoding are checked above; pin the remaining three
    // so none can be dropped from the ban without a failing test).
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, "keep-alive", "timeout=5"));
    }
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, "proxy-connection", "keep-alive"));
    }
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, "upgrade", "websocket"));
    }
    // TE is the single exception: permitted only with the exact value "trailers".
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, "te", "gzip"));
    }
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, "te", "trailers"));  // the allowed form
    }
}

RUVIA_TEST(h2_headers_duplicate_host_rejected) {
    http2_stream_state stream(1, res());
    http2_header_decode_context ctx{stream};
    RUVIA_CHECK(http2_on_decoded_initial_header(ctx, "host", "a.example"));
    RUVIA_CHECK(stream.has_host());
    RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, "host", "b.example"));  // duplicate host
}

RUVIA_TEST(h2_headers_duplicate_singleton_regular_headers_rejected) {
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, "content-type", "text/plain"));
        RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, "content-type", "application/json"));
    }
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, "range", "bytes=0-99"));
        RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, "range", "bytes=200-299"));
    }
}

RUVIA_TEST(h2_headers_repeated_etag_list_fields_accepted) {
    http2_stream_state stream(1, res());
    http2_header_decode_context ctx{stream};
    RUVIA_CHECK(http2_on_decoded_initial_header(ctx, "if-none-match", R"("old")"));
    RUVIA_CHECK(http2_on_decoded_initial_header(ctx, "if-none-match", R"("new")"));
    RUVIA_CHECK_EQ(stream.remote_header_count(), std::size_t{2});
}

RUVIA_TEST(h2_headers_duplicate_auth_and_cors_singletons_rejected) {
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, "authorization", "Bearer first"));
        RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, "authorization", "Bearer second"));
    }
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, "origin", "https://a.example"));
        RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, "origin", "https://b.example"));
    }
    {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, "access-control-request-method", "GET"));
        RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, "access-control-request-method", "POST"));
    }
}

RUVIA_TEST(h2_headers_duplicate_websocket_identity_and_user_agent_rejected) {
    struct case_value final {
        std::string_view name_;
        std::string_view first_;
        std::string_view second_;
    };
    const case_value cases[] = {
        {"sec-websocket-key", "first", "second"},
        {"sec-websocket-version", "13", "12"},
        {"user-agent", "first/1", "second/2"},
    };
    for (const auto& test : cases) {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        RUVIA_CHECK(http2_on_decoded_initial_header(ctx, test.name_, test.first_));
        RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, test.name_, test.second_));
    }
}

RUVIA_TEST(h2_headers_enforce_cors_request_field_grammar) {
    const auto accepts = [](std::string_view name, std::string_view value) {
        http2_stream_state stream(1, res());
        http2_header_decode_context ctx{stream};
        return http2_on_decoded_initial_header(ctx, name, value);
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
    http2_stream_state stream(1, res());
    http2_header_decode_context ctx{stream};
    // A non-numeric Content-Length is rejected.
    RUVIA_CHECK(!http2_on_decoded_initial_header(ctx, "content-length", "abc"));

    http2_stream_state stream2(1, res());
    http2_header_decode_context ctx2{stream2};
    RUVIA_CHECK(http2_on_decoded_initial_header(ctx2, "content-length", "42"));
    RUVIA_CHECK(http2_on_decoded_initial_header(ctx2, "content-length", "42"));
    http2_stream_state list_length(3, res());
    http2_header_decode_context list_length_ctx{list_length};
    RUVIA_CHECK(http2_on_decoded_initial_header(list_length_ctx, "content-length", "42, 42"));

    http2_stream_state conflicting_length(5, res());
    http2_header_decode_context conflicting_length_ctx{conflicting_length};
    RUVIA_CHECK(!http2_on_decoded_initial_header(conflicting_length_ctx, "content-length", "42, 43"));

    http2_stream_state repeated_conflict(7, res());
    http2_header_decode_context repeated_conflict_ctx{repeated_conflict};
    RUVIA_CHECK(http2_on_decoded_initial_header(repeated_conflict_ctx, "content-length", "42"));
    RUVIA_CHECK(!http2_on_decoded_initial_header(repeated_conflict_ctx, "content-length", "43"));

    // Split Cookie headers accumulate on the stream.
    RUVIA_CHECK(http2_on_decoded_initial_header(ctx2, "cookie", "a=1"));
    RUVIA_CHECK(http2_on_decoded_initial_header(ctx2, "cookie", "b=2"));
    RUVIA_CHECK(stream2.has_cookie());
    RUVIA_CHECK_EQ(stream2.request_cookie(), std::string_view("a=1; b=2"));
}

RUVIA_TEST(h2_headers_field_limit_counts_coalesced_cookie_lines) {
    http2_stream_state stream(1, res());
    http2_header_decode_context context_value{stream};
    for (std::size_t i = 0; i < ruvia::max_http_header_fields; ++i) {
        RUVIA_CHECK(http2_on_decoded_initial_header(context_value, "cookie", "a=1"));
    }
    RUVIA_CHECK(!http2_on_decoded_initial_header(context_value, "cookie", "a=1"));
}

RUVIA_TEST(h2_headers_expect_is_an_extensible_repeated_list) {
    http2_stream_state supported(1, res());
    http2_header_decode_context supported_context{supported};
    RUVIA_CHECK(http2_on_decoded_initial_header(supported_context, "expect", ", 100-continue,"));
    RUVIA_CHECK(http2_on_decoded_initial_header(supported_context, "expect", "100-Continue"));
    RUVIA_CHECK(supported.finalize_remote_content_head());
    const auto supported_plan = supported.expectation_plan(http_unsupported_expectation_policy::reject);
    RUVIA_CHECK(supported_plan.send_continue() != nullptr);

    RUVIA_CHECK(supported.finish_remote_content());
    const auto completed_plan = supported.expectation_plan(http_unsupported_expectation_policy::reject);
    RUVIA_CHECK(completed_plan.no_action() != nullptr);

    http2_stream_state zero_length(2, res());
    http2_header_decode_context zero_length_context{zero_length};
    RUVIA_CHECK(http2_on_decoded_initial_header(zero_length_context, "expect", "100-continue"));
    RUVIA_CHECK(http2_on_decoded_initial_header(zero_length_context, "content-length", "0"));
    RUVIA_CHECK(zero_length.finalize_remote_content_head());
    const auto zero_length_plan =
        zero_length.expectation_plan(http_unsupported_expectation_policy::reject);
    RUVIA_CHECK(zero_length_plan.no_action() != nullptr);
    RUVIA_CHECK(zero_length_plan.send_continue() == nullptr);

    http2_stream_state completed_length(4, res());
    http2_header_decode_context completed_length_context{completed_length};
    RUVIA_CHECK(http2_on_decoded_initial_header(completed_length_context, "expect", "100-continue"));
    RUVIA_CHECK(http2_on_decoded_initial_header(completed_length_context, "content-length", "1"));
    RUVIA_CHECK(completed_length.finalize_remote_content_head());
    const auto pending_length_plan =
        completed_length.expectation_plan(http_unsupported_expectation_policy::reject);
    RUVIA_CHECK(pending_length_plan.send_continue() != nullptr);
    RUVIA_CHECK(completed_length.account_remote_content(1) ==
                ruvia::detail::http2_remote_content_accounting_result::accepted);
    const auto completed_length_plan =
        completed_length.expectation_plan(http_unsupported_expectation_policy::reject);
    RUVIA_CHECK(completed_length_plan.no_action() != nullptr);

    http2_stream_state extension(3, res());
    http2_header_decode_context extension_context{extension};
    RUVIA_CHECK(
        http2_on_decoded_initial_header(extension_context, "expect", "100-continue, custom-feature"));
    RUVIA_CHECK(extension.finalize_remote_content_head());
    const auto extension_plan = extension.expectation_plan(http_unsupported_expectation_policy::reject);
    RUVIA_CHECK(extension_plan.rejection() != nullptr);
    RUVIA_CHECK(extension.request_expectations().has_continue());
    RUVIA_CHECK(extension.request_expectations().has_unsupported());

    http2_stream_state malformed(5, res());
    http2_header_decode_context malformed_context{malformed};
    RUVIA_CHECK(!http2_on_decoded_initial_header(malformed_context, "expect", "bad value"));
    RUVIA_CHECK(!malformed.request_expectations().has_continue());
    RUVIA_CHECK(!malformed.request_expectations().has_unsupported());
}

RUVIA_TEST(h2_headers_trailer_rejects_pseudo_and_invalid) {
    http2_stream_state stream(1, res());
    http2_header_decode_context ctx{stream};
    RUVIA_CHECK(http2_on_decoded_request_trailer(ctx, "x-trace-id", "abc"));
    RUVIA_CHECK(
        !http2_on_decoded_request_trailer(ctx, ":method", "GET"));  // no pseudo-headers in trailers
    RUVIA_CHECK(
        !http2_on_decoded_request_trailer(ctx, "connection", "close"));  // forbidden framing field
    RUVIA_CHECK(
        !http2_on_decoded_request_trailer(ctx, "host", "example.com"));  // routing is header-only
    RUVIA_CHECK(
        !http2_on_decoded_request_trailer(ctx, "content-length", "0"));  // framing is header-only
    RUVIA_CHECK(
        !http2_on_decoded_request_trailer(ctx, "te", "trailers"));  // connection option is header-only
    RUVIA_CHECK(!http2_on_decoded_request_trailer(ctx, "trailer", "x-checksum"));
    RUVIA_CHECK(!http2_on_decoded_request_trailer(ctx, "content-type", "text/plain"));
    RUVIA_CHECK(!http2_on_decoded_request_trailer(ctx, "origin", "https://app.example"));
    RUVIA_CHECK(!http2_on_decoded_request_trailer(ctx, "access-control-request-method", "POST"));
    RUVIA_CHECK(!http2_on_decoded_request_trailer(ctx, "access-control-request-headers", "x-one"));
    for (const std::string_view name : {"upgrade", "transfer-encoding", "proxy-connection",
             "accept-ranges", "proxy-authenticate", "proxy-authorization", "cache-control",
             "max-forwards", "set-cookie"}) {
        RUVIA_CHECK(!http2_on_decoded_request_trailer(ctx, name, "value"));
    }
}

RUVIA_TEST(h2_headers_validate_initial_trailer_field_names) {
    http2_stream_state valid(1, res());
    http2_header_decode_context valid_context{valid};
    RUVIA_CHECK(http2_on_decoded_initial_header(valid_context, "trailer", "x-checksum, x-signature"));

    http2_stream_state empty(3, res());
    http2_header_decode_context empty_context{empty};
    RUVIA_CHECK(http2_on_decoded_initial_header(empty_context, "trailer", ","));

    http2_stream_state malformed(5, res());
    http2_header_decode_context malformed_context{malformed};
    RUVIA_CHECK(!http2_on_decoded_initial_header(malformed_context, "trailer", "x-checksum, bad field"));

    http2_stream_state forbidden(7, res());
    http2_header_decode_context forbidden_context{forbidden};
    RUVIA_CHECK(!http2_on_decoded_initial_header(forbidden_context, "trailer", "Content-Length"));
}

RUVIA_TEST(h2_headers_trailer_enforces_field_count_without_storing_fields) {
    http2_stream_state stream(1, res());
    http2_header_decode_context context_value{stream};
    for (std::size_t i = 0; i < ruvia::max_http_header_fields; ++i) {
        RUVIA_CHECK(http2_on_decoded_request_trailer(context_value, "x-trace", "value"));
    }
    RUVIA_CHECK(!http2_on_decoded_request_trailer(context_value, "x-trace", "value"));
}

RUVIA_TEST(h2_headers_list_byte_limit) {
    http2_stream_state stream(1, res());
    http2_header_decode_context ctx{stream};
    RUVIA_CHECK(http2_accumulate_header_list_bytes(ctx, "accept", "text/html"));
    RUVIA_CHECK(ctx.decoded_header_list_size_.bytes() > 0);
    // A single field larger than the whole header budget is rejected.
    const std::string big(64 * 1024, 'x');
    RUVIA_CHECK(!http2_accumulate_header_list_bytes(ctx, "name", big));
}

RUVIA_TEST(h2_headers_list_byte_limit_accumulates_across_entries) {
    // The real header-flood DoS vector: many individually-legal headers that
    // together exceed the 64 KiB list budget (each entry also costs a 32-byte
    // overhead, RFC 9113 6.5.2). The accumulator must reject once the running total
    // would exceed the budget -- not merely reject a single oversized field.
    http2_stream_state stream(1, res());
    http2_header_decode_context ctx{stream};
    const std::string value(1000, 'v');  // ~1037 bytes per entry incl. name + overhead
    bool rejected = false;
    for (int i = 0; i < 200 && !rejected; ++i) {
        rejected = !http2_accumulate_header_list_bytes(ctx, "x-pad", value);
    }
    RUVIA_CHECK(rejected);                                            // the running total is bounded
    RUVIA_CHECK(ctx.decoded_header_list_size_.bytes() <= 64 * 1024);  // never exceeds the budget
}
