#include "http3/http3_quic_datagram_bridge.h"

#include <algorithm>
#include <cstddef>

namespace ruvia::detail {

ruvia::quic_address to_quic_address(const http3_quic_datagram_address& address) noexcept {
    ruvia::quic_address result_value{};
    const auto size = address.address_family_ == http3_quic_datagram_address::family::ipv4 ? 4U : 16U;
    for (std::size_t index = 0; index < size; ++index) {
        result_value.bytes_[index] = static_cast<std::byte>(address.address_[index]);
    }
    result_value.port_ = address.port_;
    result_value.scope_id_ = address.scope_id_;
    result_value.family_ = address.address_family_ == http3_quic_datagram_address::family::ipv4
                               ? ruvia::quic_address_family::ipv4
                               : ruvia::quic_address_family::ipv6;
    return result_value;
}

http3_quic_datagram_address from_quic_address(const ruvia::quic_address& address) noexcept {
    http3_quic_datagram_address result_value{};
    result_value.address_family_ = address.family_ == ruvia::quic_address_family::ipv4
                                       ? http3_quic_datagram_address::family::ipv4
                                       : http3_quic_datagram_address::family::ipv6;
    const auto size = result_value.address_family_ == http3_quic_datagram_address::family::ipv4 ? 4U : 16U;
    for (std::size_t index = 0; index < size; ++index) {
        result_value.address_[index] = std::to_integer<std::uint8_t>(address.bytes_[index]);
    }
    result_value.port_ = address.port_;
    result_value.scope_id_ = address.scope_id_;
    return result_value;
}

}  // namespace ruvia::detail
