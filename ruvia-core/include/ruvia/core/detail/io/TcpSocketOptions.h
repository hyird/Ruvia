#pragma once

#include <stdexcept>
#include <system_error>

#include <asio/ip/tcp.hpp>

#include "ruvia/core/TcpSocketOptions.h"

namespace ruvia::detail {

inline void validateTcpNoDelayPolicy(TcpNoDelayPolicy policy) {
    switch (policy) {
        case TcpNoDelayPolicy::kSystemDefault:
        case TcpNoDelayPolicy::kEnable:
            return;
        default:
            throw std::invalid_argument("TCP no-delay policy is invalid");
    }
}

inline void validateTcpKeepAlivePolicy(TcpKeepAlivePolicy policy) {
    switch (policy) {
        case TcpKeepAlivePolicy::kSystemDefault:
        case TcpKeepAlivePolicy::kEnable:
            return;
        default:
            throw std::invalid_argument("TCP keepalive policy is invalid");
    }
}

[[nodiscard]] constexpr bool tcpNoDelayEnabled(TcpNoDelayPolicy policy) noexcept {
    return policy == TcpNoDelayPolicy::kEnable;
}

[[nodiscard]] constexpr bool tcpKeepAliveEnabled(TcpKeepAlivePolicy policy) noexcept {
    return policy == TcpKeepAlivePolicy::kEnable;
}

inline void configureTcpSocketOptions(asio::ip::tcp::socket& socket, TcpNoDelayPolicy noDelay,
    TcpKeepAlivePolicy keepAlive) noexcept {
    std::error_code ignored;
    if (tcpNoDelayEnabled(noDelay)) {
        // Asio returns error_code in compatibility mode and void with
        // ASIO_NO_DEPRECATED; the output parameter is authoritative in both.
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        (void)socket.set_option(asio::ip::tcp::no_delay(true), ignored);
    }
    if (tcpKeepAliveEnabled(keepAlive)) {
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        (void)socket.set_option(asio::socket_base::keep_alive(true), ignored);
    }
}

}  // namespace ruvia::detail
