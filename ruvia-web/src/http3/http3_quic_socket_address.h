#pragma once

#include <cstdint>
#include <variant>

#include <asio/ip/udp.hpp>

#include "ruvia/http/quic_types.h"

namespace ruvia::detail {

enum class http3_quic_socket_address_error : std::uint8_t {
    unspecified_address,
    zero_port,
    ipv6_scope_not_supported,
    ipv6_link_local_not_supported,
    ipv4_mapped_ipv6_not_supported,
    unsupported_family,
};

// The single socket/protocol address conversion. Address bytes retain network
// order; ports are host order. Datagram peer/source/destination addresses must
// be concrete: received packets retain their concrete local destination.
[[nodiscard]] std::variant<ruvia::quic_address, http3_quic_socket_address_error>
to_quic_address(const asio::ip::udp::endpoint& endpoint) noexcept;

[[nodiscard]] std::variant<asio::ip::udp::endpoint, http3_quic_socket_address_error>
to_udp_endpoint(const ruvia::quic_address& address) noexcept;

}  // namespace ruvia::detail
