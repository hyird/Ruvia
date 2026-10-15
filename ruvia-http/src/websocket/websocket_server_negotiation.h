#pragma once

#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/http_header.h"

#include "websocket/http_websocket_handshake_fields.h"
#include "websocket/http_websocket_permessage_deflate.h"

namespace ruvia::detail {

struct websocket_server_negotiation_options final {
    // Server preference order. Every entry must be a nonempty, unique HTTP token.
    std::span<const std::string_view> supported_subprotocols_{};
    std::span<const http_header_view> response_headers_{};
    std::pmr::memory_resource* resource_{nullptr};
    websocket_deflate_config deflate_{};
};

// HTTP-version-independent result of one server-side websocket negotiation. HTTP/1
// and RFC 8441 consume this same immutable value for their response head, and the
// subsequent ws_connection consumes its exact deflate alternative. This prevents
// response metadata and frame RSV1 semantics from being configured separately.
// The selected subprotocol is copied into caller-selected PMR storage so this
// committed value never borrows mutable route configuration.
class websocket_server_negotiation final {
public:
    websocket_server_negotiation(const websocket_server_negotiation&) = delete;
    websocket_server_negotiation& operator=(const websocket_server_negotiation&) = delete;
    websocket_server_negotiation(websocket_server_negotiation&&) noexcept = default;
    websocket_server_negotiation& operator=(websocket_server_negotiation&&) = delete;

    [[nodiscard]] std::string_view subprotocol() const& noexcept {
        return subprotocol_;
    }
    std::string_view subprotocol() const&& = delete;

    [[nodiscard]] websocket_compression compression() const noexcept {
        return compression_;
    }

    [[nodiscard]] std::string_view extensions() const noexcept {
        return extensions_;
    }

    [[nodiscard]] std::span<const http_header> response_headers() const& noexcept {
        return response_headers_;
    }
    std::span<const http_header> response_headers() const&& = delete;

private:
    friend websocket_server_negotiation make_websocket_server_negotiation(
        const http_request&, websocket_server_negotiation_options);

    websocket_server_negotiation(std::string_view subprotocol, websocket_compression compression,
        std::span<const http_header_view> response_headers_value, std::pmr::memory_resource* resource);

    std::pmr::string subprotocol_;
    websocket_compression compression_;
    std::pmr::string extensions_;
    std::pmr::vector<http_header> response_headers_;
};

[[nodiscard]] websocket_server_negotiation make_websocket_server_negotiation(
    const http_request& request, websocket_server_negotiation_options options = {});

}  // namespace ruvia::detail
