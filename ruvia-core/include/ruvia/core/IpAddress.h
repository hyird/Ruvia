#pragma once

#include <expected>
#include <string_view>
#include <system_error>

#include <asio/ip/address.hpp>

#include "ruvia/core/detail/io/IpAddress.h"

namespace ruvia {

// Parse a complete textual IP address, rejecting embedded NUL bytes.
[[nodiscard]] inline std::expected<asio::ip::address, std::error_code> parseIpAddress(
    std::string_view text) {
    return detail::parseIpAddress(text);
}

}  // namespace ruvia
