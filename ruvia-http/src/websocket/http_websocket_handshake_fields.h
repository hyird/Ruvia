#pragma once

#include <span>
#include <string_view>

#include "ruvia/http/http_header.h"

namespace ruvia {

class http_request;

namespace detail {

[[nodiscard]] bool websocket_subprotocol_offers_valid(const http_request& request) noexcept;
[[nodiscard]] bool websocket_extension_offers_valid(const http_request& request) noexcept;
[[nodiscard]] bool websocket_protocol_offered(
    const http_request& request, std::string_view protocol) noexcept;
[[nodiscard]] std::string_view choose_websocket_subprotocol(
    const http_request& request, std::span<const std::string_view> supported) noexcept;

}  // namespace detail
}  // namespace ruvia
