#pragma once

#include <cstddef>
#include <system_error>

#include <asio/error.hpp>

namespace ruvia::detail {

// The largest UDP payload (65535 minus the UDP header): a receive buffer of
// this size truncates no datagram.
inline constexpr std::size_t http3_max_udp_payload_bytes = 65527;

// A receive error that concerns a single datagram, not the socket: ICMP
// errors of earlier sends (Windows reports them on later receives) and
// truncation. The socket keeps receiving.
[[nodiscard]] inline bool http3_ignorable_receive_error(const std::error_code& error) noexcept {
    return error == asio::error::connection_refused || error == asio::error::connection_reset ||
           error == asio::error::message_size || error == asio::error::network_unreachable ||
           error == asio::error::host_unreachable;
}

}  // namespace ruvia::detail
