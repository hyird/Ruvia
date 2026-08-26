#pragma once

#include <span>
#include <string_view>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/util/BorrowedView.h"

namespace ruvia {

class HttpRequest;

namespace detail {

[[nodiscard]] bool isValidWebSocketSubprotocolList(std::string_view protocols) noexcept;
[[nodiscard]] bool webSocketSubprotocolOffersValid(const HttpRequest& request) noexcept;
[[nodiscard]] bool webSocketExtensionOffersValid(const HttpRequest& request) noexcept;
// Sender-side counterpart for APIs that own a raw HTTP header span instead of
// an HttpRequest. It applies the same cross-field uniqueness and list grammar
// as the two request validators above, so HTTP/2 WebSocket CONNECT cannot emit
// an offer that the server path would reject.
[[nodiscard]] bool webSocketClientOfferHeadersValid(
    std::span<const HttpHeaderView> headers) noexcept;
[[nodiscard]] bool webSocketProtocolOffered(
    const HttpRequest& request, std::string_view protocol) noexcept;
[[nodiscard]] std::string_view chooseWebSocketSubprotocol(
    const HttpRequest& request, std::string_view supported) noexcept;
template <HttpTemporaryOwningCharString Supported>
std::string_view chooseWebSocketSubprotocol(const HttpRequest&, Supported&&) = delete;

}  // namespace detail
}  // namespace ruvia
