#include <cstdint>
#include <memory_resource>
#include <string_view>

#include "http2/http2_request_builder.h"
#include "request/http_request_access.h"
#include "request_header_memory_fixture.h"
#include "test_harness.h"

namespace {

using ruvia::http_known_method;
using ruvia::http_protocol_version;
using ruvia::http_request_target_form;
using ruvia::detail::http2_request_build_result;
using ruvia::detail::http2_request_builder;
using ruvia::detail::http2_stream_state;
using ruvia::detail::http_request_access;
using ruvia::detail::request_body_bytes;
using ruvia::detail::request_header_kind;

http2_stream_state make_stream() {
    return http2_stream_state(1, std::pmr::new_delete_resource());
}

[[nodiscard]] bool build_request(
    http2_stream_state& stream, ruvia::http_request& request, std::string_view body = {}) {
    const auto result_value =
        http2_request_builder::build(stream, request, std::pmr::new_delete_resource(), body);
    return result_value.built() != nullptr;
}

void check_build_failure(ruvia::testing::test_context& ruvia_ctx,
    const http2_request_build_result& result_value, ruvia::http_status_code expected_status,
    std::string_view expected_message) {
    RUVIA_CHECK(result_value.built() == nullptr);
    RUVIA_CHECK(result_value.failure() != nullptr);
    if (const auto* failure = result_value.failure()) {
        const auto error = failure->protocol_error();
        RUVIA_CHECK_EQ(error.status(), expected_status);
        RUVIA_CHECK_EQ(std::string_view(error.what()), expected_message);
    }
}

}  // namespace

RUVIA_TEST(h2_request_header_blocks_release_independently_of_retained_stream_data) {
    ruvia::test::header_memory resource;
    auto stream = make_stream();
    stream.assign_request_method("GET");
    stream.assign_request_path("/saved");
    RUVIA_CHECK(stream.append_remote_header("x-value", "handshake", request_header_kind::other));
    auto retained = http_request_access::make();
    const auto retained_build = http2_request_builder::build(stream, retained, &resource, {});
    RUVIA_CHECK(retained_build.built() != nullptr);
    const auto baseline = resource.live_bytes_;
    RUVIA_CHECK_EQ(baseline, sizeof(ruvia::http_header_view) + 1);
    for (int i = 0; i < 64; ++i) {
        {
            auto request = http_request_access::make();
            const auto build = http2_request_builder::build(stream, request, &resource, {});
            RUVIA_CHECK(build.built() != nullptr);
            RUVIA_CHECK_EQ(resource.live_bytes_, baseline * 2);
        }
        RUVIA_CHECK_EQ(resource.live_bytes_, baseline);
        RUVIA_CHECK_EQ(retained.header("x-value").value(), std::string_view("handshake"));
    }
    resource.reject_ = true;
    bool failed = false;
    try {
        auto request = http_request_access::make();
        (void)http2_request_builder::build(stream, request, &resource, {});
    } catch (const std::bad_alloc&) {
        failed = true;
    }
    RUVIA_CHECK(failed);
    RUVIA_CHECK_EQ(resource.live_bytes_, baseline);
    http_request_access::reset(retained);
    RUVIA_CHECK_EQ(resource.live_bytes_, std::size_t{0});
    RUVIA_CHECK_EQ(stream.remote_header_at(0).value_, std::string_view("handshake"));
}

RUVIA_TEST(h2_request_builder_preserves_extension_method_for_web_501) {
    auto request = http_request_access::make();
    auto stream = make_stream();
    stream.assign_request_method("PROPFIND");
    stream.assign_request_path("/dav/resource");

    RUVIA_CHECK(build_request(stream, request));
    RUVIA_CHECK_EQ(request.method(), std::string_view("PROPFIND"));
    RUVIA_CHECK(request.known_method() == http_known_method::unknown);
    RUVIA_CHECK_EQ(request.path(), std::string_view("/dav/resource"));
    RUVIA_CHECK(request.protocol_version() == http_protocol_version::http2);
}

RUVIA_TEST(h2_request_builder_uses_connection_protocol_version) {
    auto request = http_request_access::make();
    auto stream = make_stream();
    stream.assign_request_method("GET");
    stream.assign_request_path("/");

    RUVIA_CHECK(build_request(stream, request));
    RUVIA_CHECK(request.protocol_version() == http_protocol_version::http2);
    RUVIA_CHECK(request.target_form() == http_request_target_form::http2);
}

RUVIA_TEST(h2_request_builder_accepts_body_from_external_runtime_owner) {
    auto request = http_request_access::make();
    auto stream = make_stream();
    stream.assign_request_method("POST");
    stream.assign_request_path("/upload");

    RUVIA_CHECK(build_request(stream, request, "runtime-owned"));
    const auto body = request_body_bytes(request);
    RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()),
        std::string_view("runtime-owned"));
}

RUVIA_TEST(h2_request_builder_target_is_path_and_splits_query) {
    auto stream = make_stream();
    stream.assign_request_path("/search?q=hello&x=1");
    // For a non-CONNECT request the target is the :path pseudo-header verbatim.
    RUVIA_CHECK_EQ(
        http2_request_builder::request_target(stream), std::string_view("/search?q=hello&x=1"));
    // The path is everything before the first '?'.
    RUVIA_CHECK_EQ(http2_request_builder::request_path(stream), std::string_view("/search"));
}

RUVIA_TEST(h2_request_builder_path_without_query) {
    auto stream = make_stream();
    stream.assign_request_path("/index.html");
    RUVIA_CHECK_EQ(http2_request_builder::request_path(stream), std::string_view("/index.html"));
    RUVIA_CHECK_EQ(http2_request_builder::request_target(stream), std::string_view("/index.html"));
}

RUVIA_TEST(h2_request_builder_preserves_explicit_empty_non_http_path) {
    auto request = http_request_access::make();
    auto stream = make_stream();
    stream.assign_request_method("GET");
    stream.assign_request_scheme("git+ssh");
    stream.mark_scheme(0);
    stream.assign_request_path("");
    stream.mark_path();

    RUVIA_CHECK(build_request(stream, request));
    RUVIA_CHECK(request.target().empty());
    RUVIA_CHECK(request.path().empty());
    RUVIA_CHECK(request.query_string().empty());
}

RUVIA_TEST(h2_request_builder_uses_host_as_authority_when_pseudo_header_omitted) {
    auto request = http_request_access::make();
    auto stream = make_stream();
    stream.assign_request_method("GET");
    stream.assign_request_scheme("https");
    stream.mark_scheme(443);
    stream.assign_request_path("/");
    stream.mark_path();
    RUVIA_CHECK(stream.append_remote_header("host", "example.com", request_header_kind::host));
    stream.mark_host();

    RUVIA_CHECK(build_request(stream, request));
    RUVIA_CHECK_EQ(request.authority(), std::string_view("example.com"));
    RUVIA_CHECK_EQ(request.header("host").value_or(""), std::string_view("example.com"));
}

RUVIA_TEST(h2_request_builder_does_not_forge_host_when_authority_absent) {
    auto request = http_request_access::make();
    auto stream = make_stream();
    stream.assign_request_method("GET");
    stream.assign_request_scheme("git+ssh");
    stream.mark_scheme(0);
    stream.assign_request_path("");
    stream.mark_path();

    RUVIA_CHECK(build_request(stream, request));
    RUVIA_CHECK(!request.header("host").has_value());
}

RUVIA_TEST(h2_request_builder_preserves_explicit_empty_authority_as_empty_host) {
    auto request = http_request_access::make();
    auto stream = make_stream();
    stream.assign_request_method("GET");
    stream.assign_request_scheme("git+ssh");
    stream.mark_scheme(0);
    stream.assign_request_authority("");
    stream.mark_authority();
    stream.assign_request_path("");
    stream.mark_path();

    RUVIA_CHECK(build_request(stream, request));
    RUVIA_CHECK(request.header("host").has_value());
    RUVIA_CHECK(request.header("host")->empty());
}

RUVIA_TEST(h2_request_builder_rejects_explicit_empty_http_path) {
    auto request = http_request_access::make();
    auto stream = make_stream();
    stream.assign_request_method("GET");
    stream.assign_request_scheme("https");
    stream.mark_scheme(443);
    stream.assign_request_path("");
    stream.mark_path();

    check_build_failure(ruvia_ctx,
        http2_request_builder::build(stream, request, std::pmr::new_delete_resource(), {}),
        ruvia::http_status::bad_request, "invalid HTTP/2 request target");
}

RUVIA_TEST(h2_request_builder_asterisk_form_target) {
    auto stream = make_stream();
    stream.assign_request_path("*");
    // The asterisk-form target (OPTIONS *) keeps "*" as the path.
    RUVIA_CHECK_EQ(http2_request_builder::request_path(stream), std::string_view("*"));
}

RUVIA_TEST(h2_request_builder_rejects_non_options_asterisk_target) {
    auto request = http_request_access::make();
    auto stream = make_stream();
    stream.assign_request_method("GET");
    stream.assign_request_path("*");
    const auto failure =
        http2_request_builder::build(stream, request, std::pmr::new_delete_resource(), {});
    check_build_failure(
        ruvia_ctx, failure, ruvia::http_status::bad_request, "invalid HTTP/2 request target");

    auto options_request = http_request_access::make();
    auto options_stream = make_stream();
    options_stream.assign_request_method("OPTIONS");
    options_stream.assign_request_path("*");
    RUVIA_CHECK(build_request(options_stream, options_request));
    RUVIA_CHECK_EQ(options_request.path(), std::string_view("*"));
}

RUVIA_TEST(h2_request_builder_failure_owns_protocol_status_and_diagnostic) {
    auto request = http_request_access::make();

    auto missing_method = make_stream();
    missing_method.assign_request_path("/");
    check_build_failure(ruvia_ctx,
        http2_request_builder::build(missing_method, request, std::pmr::new_delete_resource(), {}),
        ruvia::http_status::bad_request, "missing HTTP/2 :method");

    auto missing_target = make_stream();
    missing_target.assign_request_method("GET");
    check_build_failure(ruvia_ctx,
        http2_request_builder::build(missing_target, request, std::pmr::new_delete_resource(), {}),
        ruvia::http_status::bad_request, "missing HTTP/2 request target");

    auto too_many_headers = make_stream();
    too_many_headers.assign_request_method("GET");
    too_many_headers.assign_request_path("/");
    too_many_headers.assign_request_authority("example.com");
    too_many_headers.mark_authority();
    for (std::size_t i = 0; i < ruvia::max_http_header_fields; ++i) {
        RUVIA_CHECK(
            too_many_headers.append_remote_header("x-test", "value", request_header_kind::other));
    }
    check_build_failure(ruvia_ctx,
        http2_request_builder::build(too_many_headers, request, std::pmr::new_delete_resource(), {}),
        ruvia::http_status::request_header_fields_too_large, "too many HTTP/2 request headers");
}

RUVIA_TEST(h2_request_builder_empty_query_after_question_mark) {
    auto stream = make_stream();
    stream.assign_request_path("/a?");
    // A trailing '?' with no query still yields the path up to it.
    RUVIA_CHECK_EQ(http2_request_builder::request_path(stream), std::string_view("/a"));
}

RUVIA_TEST(h2_request_builder_standard_connect_keeps_authority_form_target) {
    auto request = http_request_access::make();
    auto stream = make_stream();
    stream.assign_request_method("CONNECT");
    stream.assign_request_authority("proxy.example:443");
    stream.mark_authority();
    RUVIA_CHECK(stream.begin_standard_connect());

    RUVIA_CHECK_EQ(
        http2_request_builder::request_target(stream), std::string_view("proxy.example:443"));
    RUVIA_CHECK(build_request(stream, request));
    RUVIA_CHECK_EQ(request.method(), std::string_view("CONNECT"));
    RUVIA_CHECK(request.known_method() == http_known_method::connect);
    RUVIA_CHECK_EQ(request.target(), std::string_view("proxy.example:443"));
    RUVIA_CHECK_EQ(request.path(), std::string_view("proxy.example:443"));
    RUVIA_CHECK_EQ(request.header("host"), std::string_view("proxy.example:443"));
}

RUVIA_TEST(h2_request_builder_does_not_forge_host_from_generic_authority) {
    auto request = http_request_access::make();
    auto stream = make_stream();
    stream.assign_request_method("GET");
    stream.assign_request_scheme("git+ssh");
    stream.assign_request_authority("deploy:secret@example.test:9418");
    stream.mark_authority();
    stream.assign_request_path("/repository");

    RUVIA_CHECK(build_request(stream, request));
    RUVIA_CHECK(!request.header("host").has_value());
}

RUVIA_TEST(h2_request_builder_generic_extended_connect_retains_connect_method) {
    auto request = http_request_access::make();
    auto stream = make_stream();
    stream.assign_request_method("CONNECT");
    stream.set_protocol("connect-udp");
    stream.assign_request_authority("masque.example");
    stream.mark_authority();
    stream.assign_request_path("/.well-known/masque/udp?target=origin.example");
    RUVIA_CHECK(stream.begin_extended_connect());

    RUVIA_CHECK(build_request(stream, request));
    RUVIA_CHECK_EQ(request.method(), std::string_view("CONNECT"));
    RUVIA_CHECK(request.known_method() == http_known_method::connect);
    RUVIA_CHECK_EQ(request.path(), std::string_view("/.well-known/masque/udp"));
    RUVIA_CHECK_EQ(request.query_string(), std::string_view("target=origin.example"));
}

RUVIA_TEST(h2_request_builder_generic_extended_connect_preserves_empty_path) {
    auto request = http_request_access::make();
    auto stream = make_stream();
    stream.assign_request_method("CONNECT");
    stream.set_protocol("example-tunnel");
    stream.assign_request_scheme("custom+transport");
    stream.mark_scheme(0);
    stream.assign_request_authority("user:secret@example.test");
    stream.mark_authority();
    stream.assign_request_path("");
    stream.mark_path();
    RUVIA_CHECK(stream.begin_extended_connect());

    RUVIA_CHECK(build_request(stream, request));
    RUVIA_CHECK_EQ(request.method(), std::string_view("CONNECT"));
    RUVIA_CHECK(request.target().empty());
    RUVIA_CHECK(request.path().empty());
    RUVIA_CHECK(request.query_string().empty());
}

RUVIA_TEST(h2_request_builder_websocket_extended_connect_preserves_wire_method) {
    auto request = http_request_access::make();
    auto stream = make_stream();
    stream.assign_request_method("CONNECT");
    stream.set_protocol("WebSocket");
    stream.assign_request_authority("ws.example");
    stream.mark_authority();
    stream.assign_request_path("/chat");
    RUVIA_CHECK(stream.begin_extended_connect());

    RUVIA_CHECK(build_request(stream, request));
    RUVIA_CHECK_EQ(request.method(), std::string_view("CONNECT"));
    RUVIA_CHECK(request.known_method() == http_known_method::connect);
    RUVIA_CHECK_EQ(request.path(), std::string_view("/chat"));
}
