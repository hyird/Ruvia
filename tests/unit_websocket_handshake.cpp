#include "test_harness.h"

#include <string_view>

#include "net/ws/HttpWebSocketUtils.h"
#include "http/HttpRequestInternal.h"
#include "http/HttpRequestFlags.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/HttpTypes.h"

namespace {

using ruvia::HttpHeaderView;
using ruvia::HttpMethod;
using ruvia::HttpRequest;
using ruvia::detail::HttpRequestAccess;
using ruvia::detail::HttpRequestFlags;
using ruvia::detail::RequestKnownHeader;
using ruvia::detail::chooseWebSocketSubprotocol;
using ruvia::detail::isValidWebSocketRequest;
using ruvia::detail::webSocketProtocolOffered;

std::size_t slot(RequestKnownHeader known) {
    return HttpRequestAccess::knownHeaderSlot(known);
}

HttpRequest offering(std::string_view protocols) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::addHeader(request, HttpHeaderView{"Sec-WebSocket-Protocol", protocols},
                                 slot(RequestKnownHeader::kSecWebSocketProtocol));
    return request;
}

// The RFC 6455 §1.3 example key: base64 of a 16-byte nonce, so it decodes cleanly.
constexpr std::string_view kValidKey = "dGhlIHNhbXBsZSBub25jZQ==";

HttpRequest handshake(HttpMethod method, std::string_view httpVersion, std::string_view wsVersion) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setMethod(request, method);
    HttpRequestAccess::setHttpVersion(request, httpVersion);
    HttpRequestAccess::addHeader(request, HttpHeaderView{"Upgrade", "websocket"},
                                 slot(RequestKnownHeader::kUpgrade));
    HttpRequestAccess::addHeader(request, HttpHeaderView{"Sec-WebSocket-Version", wsVersion},
                                 slot(RequestKnownHeader::kSecWebSocketVersion));
    HttpRequestAccess::addHeader(request, HttpHeaderView{"Sec-WebSocket-Key", kValidKey},
                                 slot(RequestKnownHeader::kSecWebSocketKey));
    return request;
}

HttpRequestFlags validHandshakeFlags() {
    HttpRequestFlags flags;
    flags.upgrade = true;
    flags.secWebSocketKeyCount = 1;
    flags.secWebSocketVersionCount = 1;
    return flags;
}

}  // namespace

RUVIA_TEST(ws_subprotocol_negotiation_prefers_server_order) {
    const auto request = offering("chat, superchat");
    HttpRequestFlags flags;
    flags.secWebSocketProtocolCount = 1;
    // Server preference wins: the first supported token the client also offered.
    RUVIA_CHECK_EQ(chooseWebSocketSubprotocol(request, flags, "superchat, chat"),
                   std::string_view("superchat"));
    RUVIA_CHECK_EQ(chooseWebSocketSubprotocol(request, flags, "chat"), std::string_view("chat"));
    // No overlap yields no subprotocol.
    RUVIA_CHECK(chooseWebSocketSubprotocol(request, flags, "binary").empty());

    // A request offering nothing yields no subprotocol.
    HttpRequest none = HttpRequestAccess::make();
    HttpRequestAccess::reset(none);
    RUVIA_CHECK(chooseWebSocketSubprotocol(none, flags, "chat").empty());
}

RUVIA_TEST(ws_protocol_offered_matches_whole_tokens_only) {
    const auto request = offering("chat, superchat");
    RUVIA_CHECK(webSocketProtocolOffered(request, "chat"));
    RUVIA_CHECK(webSocketProtocolOffered(request, "superchat"));
    RUVIA_CHECK(!webSocketProtocolOffered(request, "super"));   // prefix, not a whole token
    RUVIA_CHECK(!webSocketProtocolOffered(request, "binary"));
}

RUVIA_TEST(ws_valid_request_requires_all_conditions) {
    const auto flags = validHandshakeFlags();

    RUVIA_CHECK(isValidWebSocketRequest(handshake(HttpMethod::kGet, "HTTP/1.1", "13"), flags));

    // Every individual requirement is necessary.
    RUVIA_CHECK(!isValidWebSocketRequest(handshake(HttpMethod::kPost, "HTTP/1.1", "13"), flags));  // not GET
    RUVIA_CHECK(!isValidWebSocketRequest(handshake(HttpMethod::kGet, "HTTP/1.0", "13"), flags));   // not 1.1
    RUVIA_CHECK(!isValidWebSocketRequest(handshake(HttpMethod::kGet, "HTTP/1.1", "8"), flags));    // version != 13

    {
        HttpRequestFlags noUpgrade = flags;
        noUpgrade.upgrade = false;  // Connection: Upgrade absent
        RUVIA_CHECK(!isValidWebSocketRequest(handshake(HttpMethod::kGet, "HTTP/1.1", "13"), noUpgrade));
    }
    {
        HttpRequestFlags duplicateKey = flags;
        duplicateKey.secWebSocketKeyCount = 2;  // Sec-WebSocket-Key must appear exactly once
        RUVIA_CHECK(!isValidWebSocketRequest(handshake(HttpMethod::kGet, "HTTP/1.1", "13"), duplicateKey));
    }
}
