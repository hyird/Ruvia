#include "ruvia/http/detail/websocket/handshake/HttpWebSocketHandshakeFields.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <utility>

#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/WebSocketHandshake.h"
#include "ruvia/http/detail/HttpHeaderAccess.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/field/HttpHeaderSectionSize.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/http/detail/util/HttpOws.h"
#include "ruvia/http/detail/util/PmrResource.h"
#include "ruvia/http/detail/websocket/handshake/HttpWebSocketAcceptKey.h"
#include "ruvia/http/detail/websocket/handshake/WebSocketServerNegotiation.h"
#include "ruvia/http/detail/websocket/handshake/WebSocketSubprotocolSet.h"
#include "ruvia/http/detail/websocket/message/HttpWebSocketPermessageDeflate.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] std::pmr::vector<HttpHeader> copyWebSocketResponseHeaders(
    std::span<const HttpHeaderView> headers, std::string_view subprotocol,
    WebSocketCompression compression, std::pmr::memory_resource* resource) {
    if (headers.size() > kMaxHttpHeaderFields - 5) {
        throw std::length_error("too many WebSocket handshake response headers");
    }
    // Account for generated fields as well. The HTTP/1 fields conservatively
    // cover HTTP/2's smaller :status + Date overhead, so both drivers share one
    // safe bound for the application field section.
    HttpHeaderSectionSize size;
    (void)size.add("upgrade", "websocket");
    (void)size.add("connection", "Upgrade");
    (void)size.add("sec-websocket-accept", "0000000000000000000000000000");
    if (!subprotocol.empty() && !size.add("sec-websocket-protocol", subprotocol)) {
        throw std::length_error("WebSocket handshake response headers are too large");
    }
    const auto extension = webSocketCompressionExtension(compression);
    if (!extension.empty() && !size.add("sec-websocket-extensions", extension)) {
        throw std::length_error("WebSocket handshake response headers are too large");
    }
    std::pmr::vector<HttpHeader> result(resource);
    result.reserve(headers.size());
    for (const auto& header : headers) {
        if (!isValidHttpHeaderName(header.name()) || !isValidHttpHeaderValue(header.value()) ||
            httpTrimOws(header.value()).size() != header.value().size()) {
            throw std::invalid_argument("invalid WebSocket handshake response header");
        }
        std::pmr::string name(header.name(), resource);
        for (char& ch : name) {
            ch = static_cast<char>(httpAsciiToLower(static_cast<unsigned char>(ch)));
        }
        if (name == "connection" || name == "upgrade" || name == "keep-alive" ||
            name == "proxy-connection" || name == "transfer-encoding" ||
            name == "content-length" || name == "trailer" || name == "te" ||
            name.starts_with("sec-websocket-")) {
            throw std::invalid_argument("WebSocket handshake controls this response header");
        }
        if (!size.add(name, header.value())) {
            throw std::length_error("WebSocket handshake response headers are too large");
        }
        result.push_back(HttpHeaderAccess::make(std::move(name), std::pmr::string(header.value(), resource)));
    }
    return result;
}

[[nodiscard]] bool appendWebSocketSubprotocolOffers(std::span<const HttpHeaderView> headers,
    WebSocketSubprotocolSet& protocols, bool& present) noexcept {
    for (const auto& header : headers) {
        if (!httpAsciiEqualsIgnoreCase(header.name(), "Sec-WebSocket-Protocol")) {
            continue;
        }
        present = true;
        if (!protocols.appendList(header.value())) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool appendWebSocketSubprotocolOffers(const HttpRequest& request,
    WebSocketSubprotocolSet& protocols, bool& present) noexcept {
    const auto headers = request.headers();
    for (std::size_t i = 0; i < headers.size(); ++i) {
        if (HttpRequestAccess::headerKind(request, i) !=
            std::to_underlying(RequestHeaderKind::kSecWebSocketProtocol)) {
            continue;
        }
        present = true;
        if (!protocols.appendList(headers[i].value())) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool webSocketSubprotocolHeaderOffersValid(
    std::span<const HttpHeaderView> headers) noexcept {
    WebSocketSubprotocolSet protocols;
    bool present = false;
    return appendWebSocketSubprotocolOffers(headers, protocols, present) &&
           (!present || !protocols.empty());
}

[[nodiscard]] bool webSocketProtocolTokenValid(std::string_view protocol) noexcept {
    if (protocol.empty()) {
        return false;
    }
    return std::ranges::all_of(
        protocol, [](char ch) noexcept { return isHttpTokenChar(static_cast<unsigned char>(ch)); });
}

void skipWebSocketExtensionOws(std::string_view value, std::size_t& cursor) noexcept {
    while (cursor < value.size() && (value[cursor] == ' ' || value[cursor] == '\t')) {
        ++cursor;
    }
}

[[nodiscard]] bool consumeWebSocketExtensionToken(
    std::string_view value, std::size_t& cursor) noexcept {
    const auto start = cursor;
    while (cursor < value.size() && isHttpTokenChar(static_cast<unsigned char>(value[cursor]))) {
        ++cursor;
    }
    return cursor != start;
}

[[nodiscard]] bool consumeWebSocketExtensionQuotedToken(
    std::string_view value, std::size_t& cursor) noexcept {
    if (cursor == value.size() || value[cursor] != '"') {
        return false;
    }
    ++cursor;
    bool decodedAny = false;
    while (cursor < value.size()) {
        auto ch = value[cursor++];
        if (ch == '"') {
            return decodedAny;
        }
        if (ch == '\\') {
            if (cursor == value.size()) {
                return false;
            }
            ch = value[cursor++];
        }
        // RFC 6455 section 4.3 narrows quoted-string extension values:
        // after quoted-pair unescaping, the result still has to be a token.
        if (!isHttpTokenChar(static_cast<unsigned char>(ch))) {
            return false;
        }
        decodedAny = true;
    }
    return false;
}

[[nodiscard]] bool appendWebSocketExtensionList(
    std::string_view value, bool& hasExtension) noexcept {
    std::size_t cursor = 0;
    while (true) {
        skipWebSocketExtensionOws(value, cursor);
        // RFC 2616's #rule permits null comma-list elements, but 1#extension
        // still requires at least one real extension across the logical field.
        while (cursor < value.size() && value[cursor] == ',') {
            ++cursor;
            skipWebSocketExtensionOws(value, cursor);
        }
        if (cursor == value.size()) {
            return true;
        }
        if (!consumeWebSocketExtensionToken(value, cursor)) {
            return false;
        }
        hasExtension = true;

        while (true) {
            skipWebSocketExtensionOws(value, cursor);
            if (cursor == value.size()) {
                return true;
            }
            if (value[cursor] == ',') {
                ++cursor;
                break;
            }
            if (value[cursor] != ';') {
                return false;
            }
            ++cursor;
            skipWebSocketExtensionOws(value, cursor);
            if (!consumeWebSocketExtensionToken(value, cursor)) {
                return false;
            }
            skipWebSocketExtensionOws(value, cursor);
            if (cursor == value.size() || value[cursor] != '=') {
                continue;
            }
            ++cursor;
            skipWebSocketExtensionOws(value, cursor);
            if (cursor == value.size()) {
                return false;
            }
            if (value[cursor] == '"') {
                if (!consumeWebSocketExtensionQuotedToken(value, cursor)) {
                    return false;
                }
            } else if (!consumeWebSocketExtensionToken(value, cursor)) {
                return false;
            }
        }
    }
}

[[nodiscard]] bool webSocketExtensionHeaderOffersValid(
    std::span<const HttpHeaderView> headers) noexcept {
    bool present = false;
    bool hasExtension = false;
    for (const auto& header : headers) {
        if (!httpAsciiEqualsIgnoreCase(header.name(), "Sec-WebSocket-Extensions")) {
            continue;
        }
        present = true;
        if (!appendWebSocketExtensionList(header.value(), hasExtension)) {
            return false;
        }
    }
    return !present || hasExtension;
}

}  // namespace

bool webSocketSubprotocolOffersValid(const HttpRequest& request) noexcept {
    WebSocketSubprotocolSet protocols;
    bool present = false;
    return appendWebSocketSubprotocolOffers(request, protocols, present) &&
           (!present || !protocols.empty());
}

bool webSocketExtensionOffersValid(const HttpRequest& request) noexcept {
    return webSocketExtensionHeaderOffersValid(request.headers());
}

bool webSocketClientOfferHeadersValid(std::span<const HttpHeaderView> headers) noexcept {
    return webSocketSubprotocolHeaderOffersValid(headers) &&
           webSocketExtensionHeaderOffersValid(headers);
}

bool webSocketProtocolOffered(const HttpRequest& request, std::string_view protocol) noexcept {
    if (!webSocketProtocolTokenValid(protocol)) {
        return false;
    }
    WebSocketSubprotocolSet protocols;
    bool present = false;
    return appendWebSocketSubprotocolOffers(request, protocols, present) && present &&
           !protocols.empty() && protocols.contains(protocol);
}

std::string_view chooseWebSocketSubprotocol(
    const HttpRequest& request, std::span<const std::string_view> supported) noexcept {
    WebSocketSubprotocolSet configured;
    for (const auto protocol : supported) {
        if (!configured.append(protocol)) {
            return {};
        }
    }

    WebSocketSubprotocolSet offered;
    bool present = false;
    if (!appendWebSocketSubprotocolOffers(request, offered, present) || !present ||
        offered.empty()) {
        return {};
    }
    for (const auto protocol : supported) {
        if (offered.contains(protocol)) {
            return protocol;
        }
    }
    return {};
}

WebSocketServerNegotiation::WebSocketServerNegotiation(std::string_view subprotocol,
    WebSocketCompression compression, std::span<const HttpHeaderView> responseHeaders,
    std::pmr::memory_resource* resource)
    : subprotocol_(subprotocol, httpPmrResourceOrDefault(resource)),
      compression_(compression),
      responseHeaders_(copyWebSocketResponseHeaders(responseHeaders, subprotocol, compression, httpPmrResourceOrDefault(resource))) {}

WebSocketServerNegotiation makeWebSocketServerNegotiation(
    const HttpRequest& request, WebSocketServerNegotiationOptions options) {
    return WebSocketServerNegotiation(
        chooseWebSocketSubprotocol(request, options.supportedSubprotocols),
        webSocketNegotiatePermessageDeflate(request), options.responseHeaders, options.resource);
}

}  // namespace ruvia::detail

namespace ruvia {

WebSocketServerHandshake makeWebSocketServerHandshake(
    const HttpRequest& request, WebSocketServerHandshakeOptions options) {
    detail::WebSocketAcceptKey accept;
    detail::encodeWebSocketAccept(
        accept, detail::requestKnownHeader(request, detail::RequestKnownHeader::kSecWebSocketKey));
    std::pmr::string subprotocol(
        detail::chooseWebSocketSubprotocol(request, options.supportedSubprotocols),
        detail::httpPmrResourceOrDefault(options.resource));
    const auto compression = detail::webSocketNegotiatePermessageDeflate(request);
    auto responseHeaders = detail::copyWebSocketResponseHeaders(options.responseHeaders,
        subprotocol, compression, detail::httpPmrResourceOrDefault(options.resource));
    return WebSocketServerHandshake(accept, std::move(subprotocol), compression, std::move(responseHeaders));
}

}  // namespace ruvia
