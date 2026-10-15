#include "http3/http3_quic_socket_address.h"

#include <algorithm>
#include <array>
#include <cstddef>

#include <asio/ip/address_v4.hpp>
#include <asio/ip/address_v6.hpp>

namespace ruvia::detail {
namespace {
using error = http3_quic_socket_address_error;

bool is_unspecified(const std::array<std::byte, 16>& bytes_value, std::size_t length) noexcept {
    return std::all_of(bytes_value.begin(), bytes_value.begin() + static_cast<std::ptrdiff_t>(length),
        [](std::byte byte) { return byte == std::byte{}; });
}

template <typename bytes_type>
void copy_address(const bytes_type& source, std::array<std::byte, 16>& target) noexcept {
    std::transform(source.begin(), source.end(), target.begin(),
        [](unsigned char byte) { return static_cast<std::byte>(byte); });
}

}  // namespace

std::variant<ruvia::quic_address, http3_quic_socket_address_error>
to_quic_address(const asio::ip::udp::endpoint& endpoint) noexcept {
    if (endpoint.port() == 0) {
        return error::zero_port;
    }
    ruvia::quic_address result;
    result.port_ = endpoint.port();
    const auto& address = endpoint.address();
    if (address.is_v4()) {
        result.family_ = ruvia::quic_address_family::ipv4;
        copy_address(address.to_v4().to_bytes(), result.bytes_);
        if (is_unspecified(result.bytes_, 4)) {
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
    result.family_ = ruvia::quic_address_family::ipv6;
    copy_address(ipv6.to_bytes(), result.bytes_);
    if (is_unspecified(result.bytes_, 16)) {
        return error::unspecified_address;
    }
    return result;
}

std::variant<asio::ip::udp::endpoint, http3_quic_socket_address_error>
to_udp_endpoint(const ruvia::quic_address& address) noexcept {
    if (address.port_ == 0) {
        return error::zero_port;
    }
    if (address.scope_id_ != 0) {
        return error::ipv6_scope_not_supported;
    }
    if (address.family_ == ruvia::quic_address_family::ipv4) {
        if (is_unspecified(address.bytes_, 4)) {
            return error::unspecified_address;
        }
        asio::ip::address_v4::bytes_type bytes_value{};
        std::transform(address.bytes_.begin(), address.bytes_.begin() + 4, bytes_value.begin(),
            [](std::byte byte) { return std::to_integer<unsigned char>(byte); });
        return asio::ip::udp::endpoint(asio::ip::address_v4(bytes_value), address.port_);
    }
    if (address.family_ != ruvia::quic_address_family::ipv6) {
        return error::unsupported_family;
    }
    asio::ip::address_v6::bytes_type bytes_value{};
    std::transform(address.bytes_.begin(), address.bytes_.end(), bytes_value.begin(),
        [](std::byte byte) { return std::to_integer<unsigned char>(byte); });
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
