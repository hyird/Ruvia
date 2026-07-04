#include "test_harness.h"

#include <memory_resource>
#include <string_view>

#include "net/http2/Http2WebSocketHandshake.h"
#include "http/HttpRequestInternal.h"
#include "ruvia/http/HttpRequest.h"

namespace {

using ruvia::HttpHeaderView;
using ruvia::HttpRequest;
using ruvia::detail::Http2StreamState;
using ruvia::detail::HttpRequestAccess;
using ruvia::detail::RequestKnownHeader;
using ruvia::detail::http2ChooseWebSocketSubprotocol;
using ruvia::detail::http2IsValidWebSocketRequest;

HttpRequest requestWith(RequestKnownHeader known, std::string_view name, std::string_view value) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::addHeader(request, HttpHeaderView{name, value},
                                 HttpRequestAccess::knownHeaderSlot(known));
    return request;
}

Http2StreamState makeStream() {
    return Http2StreamState(1, std::pmr::new_delete_resource());
}

}  // namespace

RUVIA_TEST(websocket_subprotocol_negotiation) {
    const auto request = requestWith(RequestKnownHeader::kSecWebSocketProtocol,
                                     "sec-websocket-protocol", "chat, superchat");
    // Server preference wins: the first supported token the client also offered.
    RUVIA_CHECK_EQ(http2ChooseWebSocketSubprotocol(request, "superchat, chat"),
                   std::string_view("superchat"));
    RUVIA_CHECK_EQ(http2ChooseWebSocketSubprotocol(request, "chat"), std::string_view("chat"));
    // No overlap yields no subprotocol.
    RUVIA_CHECK(http2ChooseWebSocketSubprotocol(request, "binary").empty());

    // A request offering nothing yields no subprotocol.
    HttpRequest none = HttpRequestAccess::make();
    HttpRequestAccess::reset(none);
    RUVIA_CHECK(http2ChooseWebSocketSubprotocol(none, "chat").empty());
}

RUVIA_TEST(websocket_request_validity_requires_all_conditions) {
    const auto request = requestWith(RequestKnownHeader::kSecWebSocketVersion,
                                     "sec-websocket-version", "13");

    auto valid = makeStream();
    valid.markExtendedConnectWebSocket();
    valid.markWebSocketTunnel();
    RUVIA_CHECK(http2IsValidWebSocketRequest(valid, request));

    // Missing the tunnel flag invalidates it.
    auto noTunnel = makeStream();
    noTunnel.markExtendedConnectWebSocket();
    RUVIA_CHECK(!http2IsValidWebSocketRequest(noTunnel, request));

    // A Content-Length must not be present on a WebSocket CONNECT.
    auto withContentLength = makeStream();
    withContentLength.markExtendedConnectWebSocket();
    withContentLength.markWebSocketTunnel();
    RUVIA_CHECK(withContentLength.setContentLength(5));
    RUVIA_CHECK(!http2IsValidWebSocketRequest(withContentLength, request));

    // The Sec-WebSocket-Version must be exactly 13.
    const auto badVersion = requestWith(RequestKnownHeader::kSecWebSocketVersion,
                                        "sec-websocket-version", "8");
    auto stream = makeStream();
    stream.markExtendedConnectWebSocket();
    stream.markWebSocketTunnel();
    RUVIA_CHECK(!http2IsValidWebSocketRequest(stream, badVersion));
}
