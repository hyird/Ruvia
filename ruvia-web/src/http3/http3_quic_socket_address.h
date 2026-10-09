#pragma once

#include <cstdint>
#include <variant>

#include <asio/ip/udp.hpp>

#include "http3/http3_quic_datagram_bridge.h"

namespace ruvia::detail {

enum class http3_quic_socket_address_error : std::uint8_t {
    unspecified_address,
    zero_port,
    ipv6_scope_not_supported,
    ipv6_link_local_not_supported,
    ipv4_mapped_ipv6_not_supported,
    unsupported_family,
};

// Address bytes retain network order; ports are represented in host byte order.
// Datagram peer/source/destination addresses must be concrete; bind addresses may
// be wildcard because received packets retain their concrete local destination.
[[nodiscard]] std::variant<http3_quic_datagram_address, http3_quic_socket_address_error>
to_http3_quic_datagram_address(const asio::ip::udp::endpoint& endpoint) noexcept;

[[nodiscard]] std::variant<http3_quic_datagram_address, http3_quic_socket_address_error>
to_http3_quic_bind_address(const asio::ip::udp::endpoint& endpoint) noexcept;

[[nodiscard]] std::variant<asio::ip::udp::endpoint, http3_quic_socket_address_error>
to_udp_endpoint(const http3_quic_datagram_address& address) noexcept;

}  // namespace ruvia::detail
