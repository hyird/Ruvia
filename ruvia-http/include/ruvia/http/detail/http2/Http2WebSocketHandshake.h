#pragma once

#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/detail/http2/Http2Hpack.h"
#include "ruvia/http/detail/http2/Http2ResponseHeaders.h"
#include "ruvia/http/detail/http2/Http2StreamState.h"
#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/http/detail/websocket/HttpWebSocketUtils.h"
#include "ruvia/http/detail/HeaderTokenUtils.h"
#include "ruvia/http/HttpTypes.h"

namespace ruvia::detail {

[[nodiscard]] inline std::string_view http2ChooseWebSocketSubprotocol(
    const HttpRequest& request,
    std::string_view supported) noexcept {
    return httpFindHeaderToken(supported, [&request](std::string_view protocol) noexcept {
        return webSocketProtocolOffered(request, protocol);
    });
}

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
        stream.remoteContent().withoutLength() != nullptr &&
        requestKnownHeader(request, RequestKnownHeader::kSecWebSocketVersion) == "13";
}

inline void http2EncodeWebSocketHandshakeHeaders(
    std::pmr::string& headerBlock,
    std::string_view subprotocol,
    std::string_view extensions = {}) {
    headerBlock.clear();
    HpackEncoder::encodeStatus(headerBlock, 200);
    HpackEncoder::encodeHeaderWithNameIndex(headerBlock, HpackStaticIndex::kDate, cachedDateValue());
    if (!subprotocol.empty()) {
        HpackEncoder::encodeHeader(headerBlock, "sec-websocket-protocol", subprotocol);
    }
    if (!extensions.empty()) {
        HpackEncoder::encodeHeader(headerBlock, "sec-websocket-extensions", extensions);
    }
}

}  // namespace ruvia::detail
