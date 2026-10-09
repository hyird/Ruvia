#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ruvia/http/quic_types.h"

namespace ruvia::detail {

// Web-side UDP address value. The HTTP core receives an equivalent protocol value;
// this type exists only at the socket boundary.
struct http3_quic_datagram_address final {
    enum class family : std::uint8_t { ipv4,
        ipv6 };
    family address_family_{family::ipv4};
    std::array<std::uint8_t, 16> address_{};
    std::uint16_t port_{};
    std::uint32_t scope_id_{};
};

[[nodiscard]] ruvia::quic_address to_quic_address(
    const http3_quic_datagram_address& address) noexcept;
[[nodiscard]] http3_quic_datagram_address from_quic_address(
    const ruvia::quic_address& address) noexcept;

}  // namespace ruvia::detail
