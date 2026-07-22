#pragma once

#include "ruvia/http/HttpStatus.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/http/detail/websocket/HttpWebSocketAcceptKey.h"
#include "ruvia/http/detail/websocket/WebSocketServerNegotiation.h"

#include <memory_resource>
#include <string_view>
#include <utility>

namespace ruvia::detail {

inline constexpr auto kHttpWebSocketSwitchingProtocolsPrefix = [] {
    constexpr std::string_view protocol = "HTTP/1.1 ";
    constexpr auto status =
        httpStatusCodeToken(http_status::kSwitchingProtocols);
    constexpr auto reason =
        httpReasonPhrase(http_status::kSwitchingProtocols);
    constexpr std::string_view suffix =
        "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: ";
    std::array<char,
        protocol.size() + status.size() + 1 + reason.size() + suffix.size()>
        result{};
    std::size_t cursor = 0;
    const auto append = [&result, &cursor](std::string_view part) {
        for (const char value : part) {
            result[cursor++] = value;
        }
    };
    append(protocol);
    append(httpStatusCodeTokenView(status));
    append(" ");
    append(reason);
    append(suffix);
    return result;
}();
inline constexpr std::string_view kHttpWebSocketSubprotocolHeaderPrefix =
    "Sec-WebSocket-Protocol: ";
inline constexpr std::string_view kHttpWebSocketExtensionsHeaderPrefix =
    "Sec-WebSocket-Extensions: ";
inline constexpr std::string_view kHttpCrlf = "\r\n";

class HttpWebSocketServerHandshake final {
public:
    HttpWebSocketServerHandshake(const HttpWebSocketServerHandshake&) = delete;
    HttpWebSocketServerHandshake& operator=(
        const HttpWebSocketServerHandshake&) = delete;
    HttpWebSocketServerHandshake(HttpWebSocketServerHandshake&&) noexcept = default;
    HttpWebSocketServerHandshake& operator=(
        HttpWebSocketServerHandshake&&) = delete;

    [[nodiscard]] const WebSocketServerNegotiation&
    negotiation() const & noexcept {
        return negotiation_;
    }
    [[nodiscard]] const WebSocketServerNegotiation&
    negotiation() const && = delete;

    template <typename Visitor>
    void forEachResponsePart(Visitor&& visitor) const {
        visitor(std::string_view(
            kHttpWebSocketSwitchingProtocolsPrefix.data(),
            kHttpWebSocketSwitchingProtocolsPrefix.size()));
        visitor(std::string_view(accept_.data(), accept_.size()));
        visitor(kHttpCrlf);
        if (!negotiation_.subprotocol().empty()) {
            visitor(kHttpWebSocketSubprotocolHeaderPrefix);
            visitor(negotiation_.subprotocol());
            visitor(kHttpCrlf);
        }
        if (!negotiation_.extensions().empty()) {
            visitor(kHttpWebSocketExtensionsHeaderPrefix);
            visitor(negotiation_.extensions());
            visitor(kHttpCrlf);
        }
        visitor(kHttpCrlf);
    }

private:
    friend HttpWebSocketServerHandshake makeHttpWebSocketServerHandshake(
        const HttpRequest&,
        std::string_view,
        std::pmr::memory_resource*);

    HttpWebSocketServerHandshake(
        WebSocketAcceptKey accept,
        WebSocketServerNegotiation&& negotiation) noexcept
        : accept_(accept),
          negotiation_(std::move(negotiation)) {}

    WebSocketAcceptKey accept_;
    WebSocketServerNegotiation negotiation_;
};

[[nodiscard]] inline HttpWebSocketServerHandshake makeHttpWebSocketServerHandshake(
    const HttpRequest& request,
    std::string_view supportedSubprotocols,
    std::pmr::memory_resource* resource = nullptr) {
    WebSocketAcceptKey accept;
    encodeWebSocketAccept(
        accept,
        requestKnownHeader(request, RequestKnownHeader::kSecWebSocketKey));
    return HttpWebSocketServerHandshake(
        accept,
        makeWebSocketServerNegotiation(
            request, supportedSubprotocols, resource));
}

}  // namespace ruvia::detail
