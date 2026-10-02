#include "ruvia/web/detail/http3/Http3QuicDatagramBridge.h"

#include <algorithm>
#include <cstddef>

namespace ruvia::detail {

ruvia::quic_address to_quic_address(const http3_quic_datagram_address& address) noexcept {
    ruvia::quic_address result{};
    const auto size = address.address_family == http3_quic_datagram_address::family::ipv4 ? 4U : 16U;
    for (std::size_t index = 0; index < size; ++index) {
        result.bytes[index] = static_cast<std::byte>(address.address[index]);
    }
    result.port = address.port;
    result.scope_id = address.scope_id;
    result.family = address.address_family == http3_quic_datagram_address::family::ipv4
                        ? ruvia::quic_address_family::ipv4
                        : ruvia::quic_address_family::ipv6;
    return result;
}

http3_quic_datagram_address from_quic_address(const ruvia::quic_address& address) noexcept {
    http3_quic_datagram_address result{};
    result.address_family = address.family == ruvia::quic_address_family::ipv4
                                ? http3_quic_datagram_address::family::ipv4
                                : http3_quic_datagram_address::family::ipv6;
    const auto size = result.address_family == http3_quic_datagram_address::family::ipv4 ? 4U : 16U;
    for (std::size_t index = 0; index < size; ++index) {
        result.address[index] = std::to_integer<std::uint8_t>(address.bytes[index]);
    }
    result.port = address.port;
    result.scope_id = address.scope_id;
    return result;
}

}  // namespace ruvia::detail
