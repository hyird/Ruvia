#pragma once

#include <expected>
#include <string_view>
#include <system_error>

#include <asio/ip/address.hpp>

namespace ruvia {

// Parse a complete textual IP address, rejecting embedded NUL bytes.
[[nodiscard]] std::expected<asio::ip::address, std::error_code> parseIpAddress(
    std::string_view text);

}  // namespace ruvia
