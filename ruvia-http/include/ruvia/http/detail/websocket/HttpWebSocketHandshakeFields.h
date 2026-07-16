#pragma once

#include <string_view>

namespace ruvia {

class HttpRequest;

namespace detail {

[[nodiscard]] bool isValidWebSocketSubprotocolList(
    std::string_view protocols) noexcept;
[[nodiscard]] bool webSocketSubprotocolOffersValid(
    const HttpRequest& request) noexcept;
[[nodiscard]] bool webSocketExtensionOffersValid(
    const HttpRequest& request) noexcept;
[[nodiscard]] bool webSocketProtocolOffered(
    const HttpRequest& request,
    std::string_view protocol) noexcept;
[[nodiscard]] std::string_view chooseWebSocketSubprotocol(
    const HttpRequest& request,
    std::string_view supported) noexcept;

}  // namespace detail
}  // namespace ruvia
