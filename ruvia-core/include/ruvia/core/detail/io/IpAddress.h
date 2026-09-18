#pragma once

#include <expected>
#include <string_view>
#include <system_error>

#include <asio/ip/address.hpp>

namespace ruvia::detail {

// Consumes the complete view, including checking for embedded NUL. Common
// address text uses stack storage; unusually long scope names use temporary PMR
// storage that is released before returning the independent address value.
[[nodiscard]] std::expected<asio::ip::address, std::error_code> parseIpAddress(std::string_view text);

}  // namespace ruvia::detail
