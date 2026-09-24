#pragma once

#include <cstdint>
#include <stdexcept>
#include <system_error>

#include <asio/ip/tcp.hpp>

namespace ruvia {

enum class TcpNoDelayPolicy : std::uint8_t {
    kSystemDefault,
    kEnable,
};

enum class TcpKeepAlivePolicy : std::uint8_t {
    kSystemDefault,
    kEnable,
};

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

inline void validateTcpSocketPolicies(TcpNoDelayPolicy noDelay, TcpKeepAlivePolicy keepAlive) {
    validateTcpNoDelayPolicy(noDelay);
    validateTcpKeepAlivePolicy(keepAlive);
}

inline void applyTcpSocketPolicies(asio::ip::tcp::socket& socket, TcpNoDelayPolicy noDelay,
    TcpKeepAlivePolicy keepAlive) noexcept {
    std::error_code ignored;
    if (noDelay == TcpNoDelayPolicy::kEnable) {
        (void)socket.set_option(asio::ip::tcp::no_delay(true), ignored);
    }
    if (keepAlive == TcpKeepAlivePolicy::kEnable) {
        (void)socket.set_option(asio::socket_base::keep_alive(true), ignored);
    }
}

}  // namespace ruvia
