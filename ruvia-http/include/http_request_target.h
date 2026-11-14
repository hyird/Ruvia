#pragma once

#include <string_view>

namespace ruvia {

// RFC 9110 CONNECT authority-form requires a nonempty host and explicit port.
[[nodiscard]] bool is_valid_http_connect_authority(std::string_view authority) noexcept;

// Validates an HTTP origin-form request target without allocating. The same
// protocol rule applies to server routes and outbound HTTP/websocket clients.
[[nodiscard]] bool is_valid_http_origin_form_target(std::string_view target) noexcept;

}  // namespace ruvia
