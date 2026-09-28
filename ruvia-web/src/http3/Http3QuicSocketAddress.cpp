#include "ruvia/web/detail/http3/Http3QuicSocketAddress.h"

#include <algorithm>
#include <array>

namespace ruvia::detail {
namespace {
using Error = Http3QuicSocketAddressError;

bool isUnspecified(const std::array<std::uint8_t, 16>& bytes, std::size_t length) noexcept {
    return std::all_of(bytes.begin(), bytes.begin() + length,
        [](std::uint8_t byte) { return byte == 0; });
}
}  // namespace

static std::expected<Http3QuicDatagramAddress, Http3QuicSocketAddressError>
toHttp3QuicAddress(
    const asio::ip::udp::endpoint& endpoint, bool allowUnspecified) noexcept {
    if (endpoint.port() == 0) {
        return std::unexpected(Error::kZeroPort);
    }

    Http3QuicDatagramAddress result;
    result.port = endpoint.port();
    const auto& address = endpoint.address();
    if (address.is_v4()) {
        result.family = Http3QuicDatagramAddress::Family::kIPv4;
        const auto bytes = address.to_v4().to_bytes();
        std::copy(bytes.begin(), bytes.end(), result.address.begin());
        if (!allowUnspecified && isUnspecified(result.address, bytes.size())) {
            return std::unexpected(Error::kUnspecifiedAddress);
        }
        return result;
    }
    if (!address.is_v6()) {
        return std::unexpected(Error::kUnsupportedFamily);
    }

    const auto ipv6 = address.to_v6();
    if (ipv6.is_v4_mapped()) {
        return std::unexpected(Error::kIPv4MappedIPv6NotSupported);
    }
    if (ipv6.scope_id() != 0) {
        return std::unexpected(Error::kIPv6ScopeNotSupported);
    }
    if (ipv6.is_link_local()) {
        return std::unexpected(Error::kIPv6LinkLocalNotSupported);
    }
    result.family = Http3QuicDatagramAddress::Family::kIPv6;
    const auto bytes = ipv6.to_bytes();
    std::copy(bytes.begin(), bytes.end(), result.address.begin());
    if (!allowUnspecified && isUnspecified(result.address, bytes.size())) {
        return std::unexpected(Error::kUnspecifiedAddress);
    }
    return result;
}

std::expected<Http3QuicDatagramAddress, Http3QuicSocketAddressError>
toHttp3QuicDatagramAddress(const asio::ip::udp::endpoint& endpoint) noexcept {
    return toHttp3QuicAddress(endpoint, false);
}

std::expected<Http3QuicDatagramAddress, Http3QuicSocketAddressError>
toHttp3QuicBindAddress(const asio::ip::udp::endpoint& endpoint) noexcept {
    return toHttp3QuicAddress(endpoint, true);
}

std::expected<asio::ip::udp::endpoint, Http3QuicSocketAddressError>
toHttp3UdpEndpoint(const Http3QuicDatagramAddress& address) noexcept {
    if (address.port == 0) {
        return std::unexpected(Error::kZeroPort);
    }
    if (address.scopeId != 0) {
        return std::unexpected(Error::kIPv6ScopeNotSupported);
    }

    if (address.family == Http3QuicDatagramAddress::Family::kIPv4) {
        if (isUnspecified(address.address, 4)) {
            return std::unexpected(Error::kUnspecifiedAddress);
        }
        asio::ip::address_v4::bytes_type bytes{};
        std::copy_n(address.address.begin(), bytes.size(), bytes.begin());
        return asio::ip::udp::endpoint(asio::ip::address_v4(bytes), address.port);
    }
    if (address.family != Http3QuicDatagramAddress::Family::kIPv6) {
        return std::unexpected(Error::kUnsupportedFamily);
    }

    asio::ip::address_v6::bytes_type bytes{};
    std::copy(address.address.begin(), address.address.end(), bytes.begin());
    const asio::ip::address_v6 ipv6(bytes);
    if (ipv6.is_v4_mapped()) {
        return std::unexpected(Error::kIPv4MappedIPv6NotSupported);
    }
    if (ipv6.is_link_local()) {
        return std::unexpected(Error::kIPv6LinkLocalNotSupported);
    }
    if (ipv6.is_unspecified()) {
        return std::unexpected(Error::kUnspecifiedAddress);
    }
    return asio::ip::udp::endpoint(ipv6, address.port);
}

}  // namespace ruvia::detail
