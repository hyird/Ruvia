#pragma once

#include "ruvia/core/TcpSocketOptions.h"

#include <stdexcept>

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

}  // namespace ruvia::detail
