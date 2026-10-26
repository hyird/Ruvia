#pragma once

#include <stdexcept>
#include <system_error>

#include <asio/ip/tcp.hpp>

#include "ruvia/web/client_tls_config.h"

namespace ruvia {

inline void validate_tcp_socket_policies(tcp_no_delay_policy no_delay, tcp_keep_alive_policy keep_alive) {
    switch (no_delay) {
        case tcp_no_delay_policy::system_default:
        case tcp_no_delay_policy::enable:
            break;
        default:
            throw std::invalid_argument("TCP no-delay policy is invalid");
    }
    switch (keep_alive) {
        case tcp_keep_alive_policy::system_default:
        case tcp_keep_alive_policy::enable:
            break;
        default:
            throw std::invalid_argument("TCP keepalive policy is invalid");
    }
}

[[nodiscard]] inline std::error_code apply_tcp_socket_policies(asio::ip::tcp::socket& socket,
    tcp_no_delay_policy no_delay, tcp_keep_alive_policy keep_alive) noexcept {
    std::error_code error;
    if (no_delay == tcp_no_delay_policy::enable) {
        socket.set_option(asio::ip::tcp::no_delay(true), error);
        if (error) {
            return error;
        }
    }
    if (keep_alive == tcp_keep_alive_policy::enable) {
        socket.set_option(asio::socket_base::keep_alive(true), error);
    }
    return error;
}

}  // namespace ruvia
