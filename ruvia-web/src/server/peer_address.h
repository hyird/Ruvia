#pragma once

#include <memory_resource>
#include <string>

#include <asio/ip/address.hpp>

namespace ruvia::detail {

// The peer address a connection's services name, shared by TCP and QUIC
// sessions: dotted IPv4, also for an IPv4-mapped IPv6 peer, otherwise the
// RFC 5952 IPv6 text.
void assign_peer_address(std::pmr::string& output, const asio::ip::address& address);

}  // namespace ruvia::detail
