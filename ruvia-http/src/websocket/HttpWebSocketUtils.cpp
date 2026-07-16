#include "ruvia/http/detail/websocket/HttpWebSocketUtils.h"

#include <array>

#include "ruvia/http/detail/HeaderTokenUtils.h"
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
        for (std::size_t i = 0; i < size_; ++i) {
            if (protocols_[i] == protocol) {
                return true;
            }
        }
        return false;
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
    const HttpRequest& request,
    WebSocketSubprotocolSet& protocols,
    bool& present) noexcept {
    for (const auto& header : request.headers()) {
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

[[nodiscard]] bool webSocketProtocolTokenValid(
    std::string_view protocol) noexcept {
    if (protocol.empty()) {
        return false;
    }
    for (const auto ch : protocol) {
        if (!isHttpTokenChar(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool isValidWebSocketSubprotocolList(std::string_view protocols) noexcept {
    WebSocketSubprotocolSet parsed;
    return parsed.appendList(protocols) && !parsed.empty();
}

bool webSocketSubprotocolOffersValid(const HttpRequest& request) noexcept {
    WebSocketSubprotocolSet protocols;
    bool present = false;
    return appendWebSocketSubprotocolOffers(request, protocols, present) &&
        (!present || !protocols.empty());
}

bool webSocketProtocolOffered(const HttpRequest& request, std::string_view protocol) noexcept {
    if (!webSocketProtocolTokenValid(protocol)) {
        return false;
    }
    WebSocketSubprotocolSet protocols;
    bool present = false;
    return appendWebSocketSubprotocolOffers(request, protocols, present) &&
        present && !protocols.empty() && protocols.contains(protocol);
}

std::string_view chooseWebSocketSubprotocol(
    const HttpRequest& request,
    std::string_view supported) noexcept {
    WebSocketSubprotocolSet offered;
    bool present = false;
    if (!appendWebSocketSubprotocolOffers(request, offered, present) ||
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

}  // namespace ruvia::detail
