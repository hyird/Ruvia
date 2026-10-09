#pragma once

#include <span>
#include <string_view>

#include "ruvia/http/http_header.h"

namespace ruvia {

class http_request;

namespace detail {

[[nodiscard]] bool websocket_subprotocol_offers_valid(const http_request& request) noexcept;
[[nodiscard]] bool websocket_extension_offers_valid(const http_request& request) noexcept;
// Sender-side counterpart for APIs that own a raw HTTP header span instead of
// an http_request. It applies the same cross-field uniqueness and list grammar
// as the two request validators above, so HTTP/2 websocket CONNECT cannot emit
// an offer that the server path would reject.
[[nodiscard]] bool websocket_client_offer_headers_valid(
    std::span<const http_header_view> headers) noexcept;
[[nodiscard]] bool websocket_protocol_offered(
    const http_request& request, std::string_view protocol) noexcept;
[[nodiscard]] std::string_view choose_websocket_subprotocol(
    const http_request& request, std::span<const std::string_view> supported) noexcept;

}  // namespace detail
}  // namespace ruvia
