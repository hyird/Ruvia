#pragma once

#include <optional>
#include <string_view>
#include <system_error>

#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>

namespace ruvia {

// Parse a complete textual IP address, rejecting embedded NUL bytes.
[[nodiscard]] std::optional<asio::ip::address> parse_ip_address(std::string_view text);
// Close a socket and cancel any outstanding I/O.
inline void close_socket(asio::ip::tcp::socket& socket) noexcept {
    std::error_code ignored;
    socket.cancel(ignored);
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
}

}  // namespace ruvia
