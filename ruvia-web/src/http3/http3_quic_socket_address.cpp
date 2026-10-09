#include "http3/http3_quic_socket_address.h"

#include <algorithm>
#include <array>

#include <asio/ip/address_v4.hpp>
#include <asio/ip/address_v6.hpp>

namespace ruvia::detail {
namespace {
using error = http3_quic_socket_address_error;

bool is_unspecified(const std::array<std::uint8_t, 16>& bytes_value, std::size_t length) noexcept {
    return std::all_of(bytes_value.begin(), bytes_value.begin() + length,
        [](std::uint8_t byte) { return byte == 0; });
}

std::variant<http3_quic_datagram_address, error> to_quic_address(
    const asio::ip::udp::endpoint& endpoint, bool allow_unspecified) noexcept {
    if (endpoint.port() == 0) {
        return error::zero_port;
    }

    http3_quic_datagram_address result;
    result.port_ = endpoint.port();
    const auto& address = endpoint.address();
    if (address.is_v4()) {
        result.address_family_ = http3_quic_datagram_address::family::ipv4;
        const auto bytes_value = address.to_v4().to_bytes();
        std::copy(bytes_value.begin(), bytes_value.end(), result.address_.begin());
        if (!allow_unspecified && is_unspecified(result.address_, bytes_value.size())) {
            return error::unspecified_address;
        }
        return result;
    }
    if (!address.is_v6()) {
        return error::unsupported_family;
    }

    const auto ipv6 = address.to_v6();
    if (ipv6.is_v4_mapped()) {
        return error::ipv4_mapped_ipv6_not_supported;
    }
    if (ipv6.scope_id() != 0) {
        return error::ipv6_scope_not_supported;
    }
    if (ipv6.is_link_local()) {
        return error::ipv6_link_local_not_supported;
    }
    result.address_family_ = http3_quic_datagram_address::family::ipv6;
    const auto bytes_value = ipv6.to_bytes();
    std::copy(bytes_value.begin(), bytes_value.end(), result.address_.begin());
    if (!allow_unspecified && is_unspecified(result.address_, bytes_value.size())) {
        return error::unspecified_address;
    }
    return result;
}
}  // namespace

std::variant<http3_quic_datagram_address, http3_quic_socket_address_error>
to_http3_quic_datagram_address(const asio::ip::udp::endpoint& endpoint) noexcept {
    return to_quic_address(endpoint, false);
}

std::variant<http3_quic_datagram_address, http3_quic_socket_address_error>
to_http3_quic_bind_address(const asio::ip::udp::endpoint& endpoint) noexcept {
    return to_quic_address(endpoint, true);
}

std::variant<asio::ip::udp::endpoint, http3_quic_socket_address_error>
to_udp_endpoint(const http3_quic_datagram_address& address) noexcept {
    if (address.port_ == 0) {
        return error::zero_port;
    }
    if (address.scope_id_ != 0) {
        return error::ipv6_scope_not_supported;
    }

    if (address.address_family_ == http3_quic_datagram_address::family::ipv4) {
        if (is_unspecified(address.address_, 4)) {
            return error::unspecified_address;
        }
        asio::ip::address_v4::bytes_type bytes_value{};
        std::copy_n(address.address_.begin(), bytes_value.size(), bytes_value.begin());
        return asio::ip::udp::endpoint(asio::ip::address_v4(bytes_value), address.port_);
    }
    if (address.address_family_ != http3_quic_datagram_address::family::ipv6) {
        return error::unsupported_family;
    }

    asio::ip::address_v6::bytes_type bytes_value{};
    std::copy(address.address_.begin(), address.address_.end(), bytes_value.begin());
    const asio::ip::address_v6 ipv6(bytes_value);
    if (ipv6.is_v4_mapped()) {
        return error::ipv4_mapped_ipv6_not_supported;
    }
    if (ipv6.is_link_local()) {
        return error::ipv6_link_local_not_supported;
    }
    if (ipv6.is_unspecified()) {
        return error::unspecified_address;
    }
    return asio::ip::udp::endpoint(ipv6, address.port_);
}

}  // namespace ruvia::detail
