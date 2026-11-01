#pragma once

#include <string_view>

namespace ruvia::detail {

// The User-Agent (RFC 9110 section 10.1.5) of an HTTP request or WebSocket
// handshake that does not carry the caller's own field.
inline constexpr std::string_view default_client_user_agent = "Ruvia";

}  // namespace ruvia::detail
