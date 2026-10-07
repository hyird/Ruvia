#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <memory_resource>
#include <string>
#include <system_error>

#include <asio/ip/tcp.hpp>

#include "ruvia/core/TcpSocketOptions.h"

namespace ruvia {

// Close a socket and cancel any outstanding I/O.
inline void closeSocket(asio::ip::tcp::socket& socket) noexcept {
    std::error_code ignored;
    socket.cancel(ignored);
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
}

// Apply the framework's accepted-connection TCP options.
inline void configureAcceptedSocket(asio::ip::tcp::socket& socket) noexcept {
    applyTcpSocketPolicies(socket, TcpNoDelayPolicy::kEnable, TcpKeepAlivePolicy::kSystemDefault);
}

// Format a peer address into caller-owned PMR storage.
inline void assignRemoteAddress(std::pmr::string& output, const asio::ip::address& address) {
    if (!address.is_v4()) {
        output = address.to_string();
        return;
    }
    const auto bytes = address.to_v4().to_bytes();
    std::array<char, 15> buffer;
    char* cursor = buffer.data();
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) {
            *cursor++ = '.';
        }
        const auto [ptr, ec] = std::to_chars(cursor, buffer.data() + buffer.size(), bytes[i]);
        if (ec != std::errc{}) {
            output = address.to_string();
            return;
        }
        cursor = ptr;
    }
    output.assign(buffer.data(), static_cast<std::size_t>(cursor - buffer.data()));
}

}  // namespace ruvia
