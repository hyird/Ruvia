#pragma once

#include <optional>
#include <string_view>

#include <asio/ip/address.hpp>

namespace ruvia {

// Parse a complete textual IP address, rejecting embedded NUL bytes.
[[nodiscard]] std::optional<asio::ip::address> parse_ip_address(std::string_view text);

}  // namespace ruvia
