#pragma once

#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/detail/http2/Http2Hpack.h"
#include "ruvia/http/detail/http2/Http2ResponseHeaders.h"
#include "ruvia/http/detail/http2/Http2StreamState.h"
#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/http/detail/websocket/HttpWebSocketUtils.h"
#include "ruvia/http/detail/websocket/WebSocketServerNegotiation.h"
#include "ruvia/http/HttpTypes.h"

namespace ruvia::detail {

[[nodiscard]] inline bool http2IsPendingWebSocketConnect(
    const Http2StreamState& stream) noexcept {
    const auto* pending = stream.tunnel().pending();
    return pending != nullptr &&
        pending->form() == Http2ConnectForm::kExtended &&
        stream.protocolIsWebSocket();
}

[[nodiscard]] inline bool http2IsValidWebSocketRequest(
    const Http2StreamState& stream,
    const HttpRequest& request) noexcept {
    return http2IsPendingWebSocketConnect(stream) &&
        stream.remoteContent().allowedWithoutLength() != nullptr &&
        requestKnownHeader(request, RequestKnownHeader::kSecWebSocketVersion) == "13";
}

inline void http2EncodeWebSocketHandshakeHeaders(
    std::pmr::string& headerBlock,
    const WebSocketServerNegotiation& negotiation) {
    headerBlock.clear();
    HpackEncoder::encodeStatus(headerBlock, 200);
    HpackEncoder::encodeHeaderWithNameIndex(headerBlock, HpackStaticIndex::kDate, cachedDateValue());
    if (!negotiation.subprotocol().empty()) {
        HpackEncoder::encodeHeader(
            headerBlock,
            "sec-websocket-protocol",
            negotiation.subprotocol());
    }
    if (!negotiation.extensions().empty()) {
        HpackEncoder::encodeHeader(
            headerBlock,
            "sec-websocket-extensions",
            negotiation.extensions());
    }
}

}  // namespace ruvia::detail
