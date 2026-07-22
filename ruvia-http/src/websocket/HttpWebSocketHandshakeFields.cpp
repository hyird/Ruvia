#include "ruvia/http/detail/websocket/HttpWebSocketHandshakeFields.h"

#include "ruvia/http/detail/util/PmrResource.h"
#include "ruvia/http/detail/websocket/HttpWebSocketPermessageDeflate.h"
#include "ruvia/http/detail/websocket/WebSocketServerNegotiation.h"

#include <algorithm>
#include <array>
#include <span>

#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/HttpRequest.h"

namespace ruvia::detail {
namespace {

class WebSocketSubprotocolSet final {
public:
    [[nodiscard]] bool appendList(std::string_view value) noexcept {
        while (true) {
            const auto comma = value.find(',');
            const auto token = httpTrimOws(
                comma == std::string_view::npos
                    ? value
                    : value.substr(0, comma));
            if (!token.empty() && !append(token)) {
                return false;
            }
            if (comma == std::string_view::npos) {
                return true;
            }
            value.remove_prefix(comma + 1);
        }
    }

    [[nodiscard]] bool contains(std::string_view protocol) const noexcept {
        const auto protocols = std::span(protocols_).first(size_);
        return std::ranges::find(protocols, protocol) != protocols.end();
    }

    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }

private:
    [[nodiscard]] bool append(std::string_view protocol) noexcept {
        for (const auto ch : protocol) {
            if (!isHttpTokenChar(static_cast<unsigned char>(ch))) {
                return false;
            }
        }
        if (size_ == protocols_.size() || contains(protocol)) {
            return false;
        }
        protocols_[size_++] = protocol;
        return true;
    }

    // The request parser already caps the complete header field count at 64.
    // Applying the same implementation limit to offered subprotocols keeps
    // uniqueness validation bounded and allocation-free on the handshake path.
    std::array<std::string_view, kMaxHttpHeaderFields> protocols_{};
    std::size_t size_{0};
};

[[nodiscard]] bool appendWebSocketSubprotocolOffers(
    std::span<const HttpHeaderView> headers,
    WebSocketSubprotocolSet& protocols,
    bool& present) noexcept {
    for (const auto& header : headers) {
        if (!httpAsciiEqualsIgnoreCase(
                header.name(), "Sec-WebSocket-Protocol")) {
            continue;
        }
        present = true;
        if (!protocols.appendList(header.value())) {
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

[[nodiscard]] bool webSocketProtocolTokenValid(
    std::string_view protocol) noexcept {
    if (protocol.empty()) {
        return false;
    }
    return std::ranges::all_of(protocol, [](char ch) noexcept {
        return isHttpTokenChar(static_cast<unsigned char>(ch));
    });
}

void skipWebSocketExtensionOws(
    std::string_view value,
    std::size_t& cursor) noexcept {
    while (cursor < value.size() &&
           (value[cursor] == ' ' || value[cursor] == '\t')) {
        ++cursor;
    }
}

[[nodiscard]] bool consumeWebSocketExtensionToken(
    std::string_view value,
    std::size_t& cursor) noexcept {
    const auto start = cursor;
    while (cursor < value.size() &&
           isHttpTokenChar(
               static_cast<unsigned char>(value[cursor]))) {
        ++cursor;
    }
    return cursor != start;
}

[[nodiscard]] bool consumeWebSocketExtensionQuotedToken(
    std::string_view value,
    std::size_t& cursor) noexcept {
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
    std::string_view value,
    bool& hasExtension) noexcept {
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
        if (!httpAsciiEqualsIgnoreCase(
                header.name(), "Sec-WebSocket-Extensions")) {
            continue;
        }
        present = true;
        if (!appendWebSocketExtensionList(
                header.value(), hasExtension)) {
            return false;
        }
    }
    return !present || hasExtension;
}

}  // namespace

bool isValidWebSocketSubprotocolList(std::string_view protocols) noexcept {
    WebSocketSubprotocolSet parsed;
    return parsed.appendList(protocols) && !parsed.empty();
}

bool webSocketSubprotocolOffersValid(const HttpRequest& request) noexcept {
    return webSocketSubprotocolHeaderOffersValid(request.headers());
}

bool webSocketExtensionOffersValid(const HttpRequest& request) noexcept {
    return webSocketExtensionHeaderOffersValid(request.headers());
}

bool webSocketClientOfferHeadersValid(
    std::span<const HttpHeaderView> headers) noexcept {
    return webSocketSubprotocolHeaderOffersValid(headers) &&
        webSocketExtensionHeaderOffersValid(headers);
}

bool webSocketProtocolOffered(const HttpRequest& request, std::string_view protocol) noexcept {
    if (!webSocketProtocolTokenValid(protocol)) {
        return false;
    }
    WebSocketSubprotocolSet protocols;
    bool present = false;
    return appendWebSocketSubprotocolOffers(
               request.headers(), protocols, present) &&
        present && !protocols.empty() && protocols.contains(protocol);
}

std::string_view chooseWebSocketSubprotocol(
    const HttpRequest& request,
    std::string_view supported) noexcept {
    WebSocketSubprotocolSet offered;
    bool present = false;
    if (!appendWebSocketSubprotocolOffers(
            request.headers(), offered, present) ||
        !present || offered.empty() ||
        !isValidWebSocketSubprotocolList(supported)) {
        return {};
    }
    return httpFindHeaderToken(
        supported,
        [&offered](std::string_view token) noexcept {
            return offered.contains(token);
        });
}

WebSocketServerNegotiation::WebSocketServerNegotiation(
    std::string_view subprotocol,
    WebSocketDeflateNegotiation deflate,
    std::pmr::memory_resource* resource)
    : subprotocol_(subprotocol, httpPmrResourceOrDefault(resource)),
      deflate_(deflate) {}

WebSocketServerNegotiation makeWebSocketServerNegotiation(
    const HttpRequest& request,
    std::string_view supportedSubprotocols,
    std::pmr::memory_resource* resource) {
    return WebSocketServerNegotiation(
        chooseWebSocketSubprotocol(request, supportedSubprotocols),
        webSocketNegotiatePermessageDeflate(request),
        resource);
}

}  // namespace ruvia::detail
