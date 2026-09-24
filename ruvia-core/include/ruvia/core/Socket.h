#pragma once

#include <memory_resource>

#include <asio/ip/tcp.hpp>

#include "ruvia/core/detail/io/SocketUtils.h"

namespace ruvia {

// Close a socket and cancel any outstanding I/O.
inline void closeSocket(asio::ip::tcp::socket& socket) noexcept {
    detail::closeSocket(socket);
}

// Apply the framework's accepted-connection TCP options.
inline void configureAcceptedSocket(asio::ip::tcp::socket& socket) noexcept {
    detail::configureAcceptedSocket(socket);
}

// Format a peer address into caller-owned PMR storage.
inline void assignRemoteAddress(
    std::pmr::string& output, const asio::ip::address& address) {
    detail::assignRemoteAddress(output, address);
}

}  // namespace ruvia
