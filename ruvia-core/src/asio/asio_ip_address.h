#pragma once

#include <optional>
#include <string_view>

#include <asio/ip/address.hpp>

namespace ruvia {

// Parse a complete textual IP address, rejecting embedded NUL bytes.
[[nodiscard]] std::optional<asio::ip::address> try_parse_asio_ip_address(std::string_view text) noexcept;

}  // namespace ruvia
