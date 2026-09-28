#pragma once

#include <cstdint>
#include <expected>

#include <asio/ip/udp.hpp>

#include "ruvia/web/detail/http3/Http3QuicDatagramBridge.h"

namespace ruvia::detail {

enum class Http3QuicSocketAddressError : std::uint8_t {
    kUnspecifiedAddress,
    kZeroPort,
    kIPv6ScopeNotSupported,
    kIPv6LinkLocalNotSupported,
    kIPv4MappedIPv6NotSupported,
    kUnsupportedFamily,
};

// Address bytes retain network order; ports are represented in host byte order.
// Datagram peer/source/destination addresses must be concrete; bind addresses may
// be wildcard because received packets retain their concrete local destination.
[[nodiscard]] std::expected<Http3QuicDatagramAddress, Http3QuicSocketAddressError>
toHttp3QuicDatagramAddress(const asio::ip::udp::endpoint& endpoint) noexcept;

[[nodiscard]] std::expected<Http3QuicDatagramAddress, Http3QuicSocketAddressError>
toHttp3QuicBindAddress(const asio::ip::udp::endpoint& endpoint) noexcept;

[[nodiscard]] std::expected<asio::ip::udp::endpoint, Http3QuicSocketAddressError>
toHttp3UdpEndpoint(const Http3QuicDatagramAddress& address) noexcept;

}  // namespace ruvia::detail
