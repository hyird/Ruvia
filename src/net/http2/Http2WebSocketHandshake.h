#pragma once

#include <memory_resource>
#include <string>
#include <string_view>

#include "Http2Hpack.h"
#include "Http2ResponseHeaders.h"
#include "Http2StreamState.h"
#include "../../http/HttpRequestInternal.h"
#include "../ws/HttpWebSocketUtils.h"
#include "../../http/HeaderTokenUtils.h"
#include "ruvia/http/HttpTypes.h"

namespace ruvia::detail {

[[nodiscard]] inline std::string_view http2ChooseWebSocketSubprotocol(
    const HttpRequest& request,
    std::string_view supported) noexcept {
    return httpFindHeaderToken(supported, [&request](std::string_view protocol) noexcept {
        return webSocketProtocolOffered(request, protocol);
    });
}

[[nodiscard]] inline bool http2IsValidWebSocketRequest(
    const Http2StreamState& stream,
    const HttpRequest& request) noexcept {
    return stream.extendedConnectWebSocket() &&
        stream.webSocketTunnel() &&
        !stream.hasContentLength() &&
        requestKnownHeader(request, RequestKnownHeader::kSecWebSocketVersion) == "13";
}

inline void http2EncodeWebSocketHandshakeHeaders(
    std::pmr::string& headerBlock,
    std::string_view subprotocol) {
    headerBlock.clear();
    HpackEncoder::encodeStatus(headerBlock, 200);
    HpackEncoder::encodeHeaderWithNameIndex(headerBlock, HpackStaticIndex::kServer, "ruvia");
    HpackEncoder::encodeHeaderWithNameIndex(headerBlock, HpackStaticIndex::kDate, cachedDateValue());
    if (!subprotocol.empty()) {
        HpackEncoder::encodeHeader(headerBlock, "sec-websocket-protocol", subprotocol);
    }
}

}  // namespace ruvia::detail
