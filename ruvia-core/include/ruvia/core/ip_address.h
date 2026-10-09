#pragma once

#include <string_view>
#include <system_error>
#include <variant>

#include <asio/ip/address.hpp>

namespace ruvia {

// Parse a complete textual IP address, rejecting embedded NUL bytes.
[[nodiscard]] std::variant<asio::ip::address, std::error_code> parse_ip_address(
    std::string_view text);

}  // namespace ruvia
