#pragma once

#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/detail/http2/hpack/Http2Hpack.h"
#include "ruvia/http/detail/http2/message/Http2RemoteReceiveSemantics.h"
#include "ruvia/http/detail/http2/message/Http2ResponseHeaders.h"
#include "ruvia/http/detail/http2/stream/Http2StreamState.h"
#include "ruvia/http/detail/util/AsciiCase.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/http/detail/websocket/handshake/HttpWebSocketHandshakeFields.h"
#include "ruvia/http/WebSocketHandshake.h"
#include "ruvia/http/detail/websocket/handshake/WebSocketHandshakeValidationAccess.h"
#include "ruvia/http/detail/websocket/handshake/WebSocketServerNegotiation.h"
#include "ruvia/http/HttpRequest.h"

namespace ruvia::detail {

[[nodiscard]] inline bool http2IsPendingWebSocketConnect(const Http2StreamState& stream) noexcept {
    const auto* pending = stream.tunnel().pending();
    return pending != nullptr && pending->form() == Http2ConnectForm::kExtended &&
           stream.protocolIsWebSocket();
}

[[nodiscard]] inline WebSocketHandshakeValidationResult validateHttp2WebSocketHandshake(
    const Http2StreamState& stream, const HttpRequest& request) noexcept {
    if (!http2IsPendingWebSocketConnect(stream) || !http2RemoteFinalHeadDecoded(stream) ||
        http2RemotePeerHalfClosed(stream) ||
        stream.remoteContent().allowedWithoutLength() == nullptr) {
        return WebSocketHandshakeValidationResultAccess::invalidRequest();
    }

    std::size_t versionCount = 0;
    std::string_view version;
    for (const auto& header : request.headers()) {
        if (httpAsciiEqualsIgnoreCase(header.name(), "Sec-WebSocket-Version")) {
            version = header.value();
            ++versionCount;
        }
    }
    if (versionCount != 1) {
        return WebSocketHandshakeValidationResultAccess::invalidRequest();
    }
    if (!webSocketSubprotocolOffersValid(request)) {
        return WebSocketHandshakeValidationResultAccess::invalidRequest();
    }
    if (!webSocketExtensionOffersValid(request)) {
        return WebSocketHandshakeValidationResultAccess::invalidRequest();
    }
    if (version != "13") {
        return WebSocketHandshakeValidationResultAccess::unsupportedVersion();
    }
    return WebSocketHandshakeValidationResultAccess::accepted();
}

inline void http2EncodeWebSocketHandshakeHeaders(
    std::pmr::string& headerBlock, const WebSocketServerNegotiation& negotiation) {
    try {
        headerBlock.clear();
        HpackEncoder::encodeStatus(headerBlock, http_status::kOk);
        HpackEncoder::encodeHeaderWithNameIndex(
            headerBlock, HpackStaticIndex::kDate, cachedDateValue());
        if (!negotiation.subprotocol().empty()) {
            HpackEncoder::encodeHeader(
                headerBlock, "sec-websocket-protocol", negotiation.subprotocol());
        }
        if (!negotiation.extensions().empty()) {
            HpackEncoder::encodeHeader(
                headerBlock, "sec-websocket-extensions", negotiation.extensions());
        }
    } catch (...) {
        headerBlock.clear();
        throw;
    }
}

}  // namespace ruvia::detail
