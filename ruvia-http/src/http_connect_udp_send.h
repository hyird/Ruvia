#pragma once

#include <optional>

#include "ruvia/http/http_connect_udp.h"

namespace ruvia::detail {
// Sender-side CONNECT-UDP request check: the request validation plus exactly one
// Capsule-Protocol: ?1 field (RFC 9297 section 3.4 SHOULD, kept mandatory here).
[[nodiscard]] std::optional<http_connect_udp_error> validate_http_connect_udp_request_to_send(
    http_connect_udp_request_view request) noexcept;
}  // namespace ruvia::detail
