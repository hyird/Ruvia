#include "test_harness.h"

#include <memory_resource>
#include <string_view>

#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/http/detail/http2/Http2RequestBuilder.h"

namespace {

using ruvia::HttpKnownMethod;
using ruvia::HttpProtocolVersion;
using ruvia::detail::Http2RequestBuilder;
using ruvia::detail::Http2StreamState;
using ruvia::detail::HttpRequestAccess;

Http2StreamState makeStream() {
    return Http2StreamState(1, std::pmr::new_delete_resource());
}

}  // namespace

RUVIA_TEST(h2_request_builder_route_method_is_known_wire_method_when_not_ws_connect) {
    auto stream = makeStream();
    stream.assignRequestMethod("POST");
    RUVIA_CHECK(Http2RequestBuilder::routeMethod(stream) == HttpKnownMethod::kPost);
    stream.assignRequestMethod("DELETE");
    RUVIA_CHECK(Http2RequestBuilder::routeMethod(stream) == HttpKnownMethod::kDelete);
}

RUVIA_TEST(h2_request_builder_preserves_extension_method_for_web_501) {
    auto request = HttpRequestAccess::make();
    auto stream = makeStream();
    stream.assignRequestMethod("PROPFIND");
    stream.assignRequestPath("/dav/resource");

    RUVIA_CHECK(Http2RequestBuilder::routeMethod(stream) == HttpKnownMethod::kUnknown);
    RUVIA_CHECK(Http2RequestBuilder::build(
        stream, request, std::pmr::new_delete_resource()));
    RUVIA_CHECK_EQ(request.method(), std::string_view("PROPFIND"));
    RUVIA_CHECK(request.knownMethod() == HttpKnownMethod::kUnknown);
    RUVIA_CHECK_EQ(request.path(), std::string_view("/dav/resource"));
    RUVIA_CHECK(
        request.protocolVersion() == HttpProtocolVersion::kHttp2);
}

RUVIA_TEST(h2_request_builder_uses_connection_protocol_version) {
    auto request = HttpRequestAccess::make();
    auto stream = makeStream();
    stream.assignRequestMethod("GET");
    stream.assignRequestPath("/");

    RUVIA_CHECK(Http2RequestBuilder::build(
        stream, request, std::pmr::new_delete_resource()));
    RUVIA_CHECK(
        request.protocolVersion() == HttpProtocolVersion::kHttp2);
}

RUVIA_TEST(h2_request_builder_target_is_path_and_splits_query) {
    auto stream = makeStream();
    stream.assignRequestPath("/search?q=hello&x=1");
    // For a non-CONNECT request the target is the :path pseudo-header verbatim.
    RUVIA_CHECK_EQ(Http2RequestBuilder::requestTarget(stream),
                   std::string_view("/search?q=hello&x=1"));
    // The path is everything before the first '?'.
    RUVIA_CHECK_EQ(Http2RequestBuilder::requestPath(stream), std::string_view("/search"));
}

RUVIA_TEST(h2_request_builder_path_without_query) {
    auto stream = makeStream();
    stream.assignRequestPath("/index.html");
    RUVIA_CHECK_EQ(Http2RequestBuilder::requestPath(stream), std::string_view("/index.html"));
    RUVIA_CHECK_EQ(Http2RequestBuilder::requestTarget(stream), std::string_view("/index.html"));
}

RUVIA_TEST(h2_request_builder_asterisk_form_target) {
    auto stream = makeStream();
    stream.assignRequestPath("*");
    // The asterisk-form target (OPTIONS *) keeps "*" as the path.
    RUVIA_CHECK_EQ(Http2RequestBuilder::requestPath(stream), std::string_view("*"));
}

RUVIA_TEST(h2_request_builder_rejects_non_options_asterisk_target) {
    auto request = HttpRequestAccess::make();
    auto stream = makeStream();
    stream.assignRequestMethod("GET");
    stream.assignRequestPath("*");
    RUVIA_CHECK(!Http2RequestBuilder::build(stream, request, std::pmr::new_delete_resource()));

    auto optionsRequest = HttpRequestAccess::make();
    auto optionsStream = makeStream();
    optionsStream.assignRequestMethod("OPTIONS");
    optionsStream.assignRequestPath("*");
    RUVIA_CHECK(Http2RequestBuilder::build(optionsStream, optionsRequest, std::pmr::new_delete_resource()));
    RUVIA_CHECK_EQ(optionsRequest.path(), std::string_view("*"));
}

RUVIA_TEST(h2_request_builder_empty_query_after_question_mark) {
    auto stream = makeStream();
    stream.assignRequestPath("/a?");
    // A trailing '?' with no query still yields the path up to it.
    RUVIA_CHECK_EQ(Http2RequestBuilder::requestPath(stream), std::string_view("/a"));
}

RUVIA_TEST(h2_request_builder_standard_connect_keeps_authority_form_target) {
    auto request = HttpRequestAccess::make();
    auto stream = makeStream();
    stream.assignRequestMethod("CONNECT");
    stream.assignRequestAuthority("proxy.example:443");
    RUVIA_CHECK(stream.beginStandardConnect());

    RUVIA_CHECK(Http2RequestBuilder::routeMethod(stream) == HttpKnownMethod::kConnect);
    RUVIA_CHECK_EQ(
        Http2RequestBuilder::requestTarget(stream),
        std::string_view("proxy.example:443"));
    RUVIA_CHECK(Http2RequestBuilder::build(
        stream, request, std::pmr::new_delete_resource()));
    RUVIA_CHECK_EQ(request.method(), std::string_view("CONNECT"));
    RUVIA_CHECK(request.knownMethod() == HttpKnownMethod::kConnect);
    RUVIA_CHECK_EQ(request.target(), std::string_view("proxy.example:443"));
    RUVIA_CHECK_EQ(request.path(), std::string_view("proxy.example:443"));
    RUVIA_CHECK_EQ(request.header("host"), std::string_view("proxy.example:443"));
}

RUVIA_TEST(h2_request_builder_generic_extended_connect_retains_connect_method) {
    auto request = HttpRequestAccess::make();
    auto stream = makeStream();
    stream.assignRequestMethod("CONNECT");
    stream.setProtocol("connect-udp");
    stream.assignRequestAuthority("masque.example");
    stream.assignRequestPath("/.well-known/masque/udp?target=origin.example");
    RUVIA_CHECK(stream.beginExtendedConnect());

    RUVIA_CHECK(Http2RequestBuilder::routeMethod(stream) == HttpKnownMethod::kConnect);
    RUVIA_CHECK(Http2RequestBuilder::build(
        stream, request, std::pmr::new_delete_resource()));
    RUVIA_CHECK_EQ(request.method(), std::string_view("CONNECT"));
    RUVIA_CHECK(request.knownMethod() == HttpKnownMethod::kConnect);
    RUVIA_CHECK_EQ(
        request.path(), std::string_view("/.well-known/masque/udp"));
    RUVIA_CHECK_EQ(
        request.queryString(), std::string_view("target=origin.example"));
}

RUVIA_TEST(h2_request_builder_websocket_extended_connect_maps_only_route_method) {
    auto request = HttpRequestAccess::make();
    auto stream = makeStream();
    stream.assignRequestMethod("CONNECT");
    stream.setProtocol("WebSocket");
    stream.assignRequestAuthority("ws.example");
    stream.assignRequestPath("/chat");
    RUVIA_CHECK(stream.beginExtendedConnect());

    RUVIA_CHECK(Http2RequestBuilder::routeMethod(stream) == HttpKnownMethod::kGet);
    RUVIA_CHECK(Http2RequestBuilder::build(
        stream, request, std::pmr::new_delete_resource()));
    RUVIA_CHECK_EQ(request.method(), std::string_view("CONNECT"));
    RUVIA_CHECK(request.knownMethod() == HttpKnownMethod::kConnect);
    RUVIA_CHECK_EQ(request.path(), std::string_view("/chat"));
}
