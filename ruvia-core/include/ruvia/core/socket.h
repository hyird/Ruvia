#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <memory_resource>
#include <string>
#include <system_error>

#include <asio/ip/tcp.hpp>

#include "ruvia/core/tcp_socket_options.h"

namespace ruvia {

// Close a socket and cancel any outstanding I/O.
inline void close_socket(asio::ip::tcp::socket& socket) noexcept {
    std::error_code ignored;
    socket.cancel(ignored);
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
}

// Apply the framework's accepted-connection TCP options.
inline void configure_accepted_socket(asio::ip::tcp::socket& socket) noexcept {
    apply_tcp_socket_policies(socket, tcp_no_delay_policy::enable, tcp_keep_alive_policy::system_default);
}

// Format a peer address into caller-owned PMR storage.
inline void assign_remote_address(std::pmr::string& output, const asio::ip::address& address) {
    if (!address.is_v4()) {
        output = address.to_string();
        return;
    }
    const auto bytes_value = address.to_v4().to_bytes();
    std::array<char, 15> buffer;
    char* cursor_value = buffer.data();
    for (std::size_t i = 0; i < bytes_value.size(); ++i) {
        if (i != 0) {
            *cursor_value++ = '.';
        }
        const auto [ptr, ec] = std::to_chars(cursor_value, buffer.data() + buffer.size(), bytes_value[i]);
        if (ec != std::errc{}) {
            output = address.to_string();
            return;
        }
        cursor_value = ptr;
    }
    output.assign(buffer.data(), static_cast<std::size_t>(cursor_value - buffer.data()));
}

}  // namespace ruvia
