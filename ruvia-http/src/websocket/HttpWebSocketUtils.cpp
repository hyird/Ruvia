#include "ruvia/http/detail/websocket/HttpWebSocketUtils.h"

#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/http/detail/HeaderTokenUtils.h"
#include "ruvia/http/HttpRequest.h"

namespace ruvia::detail {

bool webSocketProtocolOffered(const HttpRequest& request, std::string_view protocol) noexcept {
    if (httpHasExactToken(requestKnownHeader(request, RequestKnownHeader::kSecWebSocketProtocol), protocol)) {
        return true;
    }
    for (const auto& header : request.headers()) {
        if (detail::httpAsciiEqualsIgnoreCase(header.name(), "Sec-WebSocket-Protocol") &&
            httpHasExactToken(header.value(), protocol)) {
            return true;
        }
    }
    return false;
}

std::string_view chooseWebSocketSubprotocol(
    const HttpRequest& request,
    std::string_view supported) noexcept {
    return httpFindHeaderToken(supported, [&request](std::string_view token) noexcept {
        return webSocketProtocolOffered(request, token);
    });
}

}  // namespace ruvia::detail
