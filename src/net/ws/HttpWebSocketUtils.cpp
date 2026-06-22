#include "HttpWebSocketUtils.h"

#include "../../http/HttpRequestInternal.h"
#include "../../http/HeaderTokenUtils.h"
#include "ruvia/http/HttpTypes.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] bool protocolOffered(
    const HttpRequest& request,
    const HttpRequestFlags& flags,
    std::string_view protocol) noexcept {
    if (httpHasExactToken(requestKnownHeader(request, RequestKnownHeader::kSecWebSocketProtocol), protocol)) {
        return true;
    }
    if (flags.secWebSocketProtocolCount <= 1) {
        return false;
    }
    return webSocketProtocolOffered(request, protocol);
}

}  // namespace

bool webSocketProtocolOffered(const HttpRequest& request, std::string_view protocol) noexcept {
    if (httpHasExactToken(requestKnownHeader(request, RequestKnownHeader::kSecWebSocketProtocol), protocol)) {
        return true;
    }
    for (const auto& header : request.headers()) {
        if (detail::httpAsciiEqualsIgnoreCase(header.name, "Sec-WebSocket-Protocol") &&
            httpHasExactToken(header.value, protocol)) {
            return true;
        }
    }
    return false;
}

std::string_view chooseWebSocketSubprotocol(
    const HttpRequest& request,
    const HttpRequestFlags& flags,
    std::string_view supported) noexcept {
    return httpFindHeaderToken(supported, [&request, &flags](std::string_view token) noexcept {
        return protocolOffered(request, flags, token);
    });
}

}  // namespace ruvia::detail
