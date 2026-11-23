#pragma once

#include <array>
#include <string>

#include "asio_ip_endpoint.h"

namespace ruvia::detail {

using ::ruvia::to_asio_address;
using ::ruvia::to_asio_tcp_endpoint;
using ::ruvia::to_asio_udp_endpoint;
using ::ruvia::to_ip_address;
using ::ruvia::to_ip_endpoint;

// The peer address a connection's services name, shared by TCP and QUIC
// sessions: dotted IPv4, also for an IPv4-mapped IPv6 peer, otherwise the
// RFC 5952 IPv6 text.
inline void assign_peer_address(std::string& output, const asio::ip::address& address) {
    std::array<char, ip_address::max_text_size> buffer;
    output.assign(to_ip_address(address).normalized_ipv4().format_to(buffer));
}

}  // namespace ruvia::detail
