#pragma once
#include <cstddef>
#include <span>
#include <string_view>

#include "ruvia/http/detail/field/request_header_kind.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/websocket_handshake.h"
#include "ruvia/http/websocket_subprotocol_set.h"

namespace ruvia {

class http_request;

namespace detail {

[[nodiscard]] bool append_websocket_extension_list(
    std::string_view value, bool& has_extension) noexcept;
[[nodiscard]] std::string_view choose_websocket_subprotocol(
    const websocket_subprotocol_set& offered,
    std::span<const std::string_view> supported) noexcept;
[[nodiscard]] std::string_view choose_websocket_subprotocol(
    const http_request& request, std::span<const std::string_view> supported) noexcept;

class websocket_request_header_validator final {
public:
    [[nodiscard]] bool observe_header(request_header_kind kind, std::string_view value) noexcept;
    [[nodiscard]] bool offers_valid() const noexcept;
    [[nodiscard]] bool client_offers_valid() const noexcept;
    [[nodiscard]] websocket_handshake_validation_result finish(
        websocket_handshake_validation_options options = {}) const noexcept;

private:
    websocket_subprotocol_set protocols_{};
    std::string_view version_{};
    std::size_t version_count_{0};
    bool valid_{true};
    bool protocols_present_{false};
    bool extensions_present_{false};
    bool has_extension_{false};
};
}  // namespace detail
}  // namespace ruvia
