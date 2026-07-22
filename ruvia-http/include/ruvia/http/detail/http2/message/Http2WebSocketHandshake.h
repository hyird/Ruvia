#pragma once

#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/detail/http2/hpack/Http2Hpack.h"
#include "ruvia/http/detail/http2/message/Http2ResponseHeaders.h"
#include "ruvia/http/detail/http2/stream/Http2StreamState.h"
#include "ruvia/http/detail/util/AsciiCase.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/http/detail/websocket/handshake/HttpWebSocketHandshakeFields.h"
#include "ruvia/http/detail/websocket/handshake/HttpWebSocketHandshakeValidation.h"
#include "ruvia/http/detail/websocket/handshake/WebSocketServerNegotiation.h"
#include "ruvia/http/HttpRequest.h"

namespace ruvia::detail {

[[nodiscard]] inline bool http2IsPendingWebSocketConnect(
    const Http2StreamState& stream) noexcept {
    const auto* pending = stream.tunnel().pending();
    return pending != nullptr &&
        pending->form() == Http2ConnectForm::kExtended &&
        stream.protocolIsWebSocket();
}

[[nodiscard]] inline HttpWebSocketHandshakeValidationResult
validateHttp2WebSocketHandshake(
    const Http2StreamState& stream,
    const HttpRequest& request) noexcept {
    if (!http2IsPendingWebSocketConnect(stream) ||
        stream.remoteContent().allowedWithoutLength() == nullptr) {
        return HttpWebSocketHandshakeValidationResult::makeInvalidRequest();
    }

    std::size_t versionCount = 0;
    std::string_view version;
    for (const auto& header : request.headers()) {
        if (httpAsciiEqualsIgnoreCase(
                header.name(),
                "Sec-WebSocket-Version")) {
            version = header.value();
            ++versionCount;
        }
    }
    if (versionCount != 1) {
        return HttpWebSocketHandshakeValidationResult::makeInvalidRequest();
    }
    if (!webSocketSubprotocolOffersValid(request)) {
        return HttpWebSocketHandshakeValidationResult::makeInvalidRequest();
    }
    if (!webSocketExtensionOffersValid(request)) {
        return HttpWebSocketHandshakeValidationResult::makeInvalidRequest();
    }
    if (version != "13") {
        return HttpWebSocketHandshakeValidationResult::
            makeUnsupportedVersion();
    }
    return HttpWebSocketHandshakeValidationResult::makeAccepted();
}

inline void http2EncodeWebSocketHandshakeHeaders(
    std::pmr::string& headerBlock,
    const WebSocketServerNegotiation& negotiation) {
    headerBlock.clear();
    HpackEncoder::encodeStatus(headerBlock, http_status::kOk);
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
