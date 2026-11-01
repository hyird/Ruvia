#pragma once

#include <stdexcept>
#include <system_error>

#include <asio/ip/tcp.hpp>

#include "ruvia/web/client_tcp_config.h"

namespace ruvia::detail {

inline void validate_client_tcp_config(const client_tcp_config& config) {
    switch (config.no_delay_) {
        case tcp_no_delay_policy::system_default:
        case tcp_no_delay_policy::enable:
            break;
        default:
            throw std::invalid_argument("TCP no-delay policy is invalid");
    }
    switch (config.keep_alive_) {
        case tcp_keep_alive_policy::system_default:
        case tcp_keep_alive_policy::enable:
            break;
        default:
            throw std::invalid_argument("TCP keepalive policy is invalid");
    }
}

// Returns the error of the first enabled policy that cannot be applied.
[[nodiscard]] inline std::error_code apply_tcp_socket_policies(
    asio::ip::tcp::socket& socket, const client_tcp_config& config) noexcept {
    std::error_code error;
    if (config.no_delay_ == tcp_no_delay_policy::enable) {
        socket.set_option(asio::ip::tcp::no_delay(true), error);
        if (error) {
            return error;
        }
    }
    if (config.keep_alive_ == tcp_keep_alive_policy::enable) {
        socket.set_option(asio::socket_base::keep_alive(true), error);
    }
    return error;
}

}  // namespace ruvia::detail
