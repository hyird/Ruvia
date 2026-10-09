#pragma once

#include <cstdint>
#include <stdexcept>
#include <system_error>

#include <asio/ip/tcp.hpp>

namespace ruvia {

enum class tcp_no_delay_policy : std::uint8_t {
    system_default,
    enable,
};

enum class tcp_keep_alive_policy : std::uint8_t {
    system_default,
    enable,
};

inline void validate_tcp_no_delay_policy(tcp_no_delay_policy policy) {
    switch (policy) {
        case tcp_no_delay_policy::system_default:
        case tcp_no_delay_policy::enable:
            return;
        default:
            throw std::invalid_argument("TCP no-delay policy is invalid");
    }
}

inline void validate_tcp_keep_alive_policy(tcp_keep_alive_policy policy) {
    switch (policy) {
        case tcp_keep_alive_policy::system_default:
        case tcp_keep_alive_policy::enable:
            return;
        default:
            throw std::invalid_argument("TCP keepalive policy is invalid");
    }
}

inline void validate_tcp_socket_policies(tcp_no_delay_policy no_delay, tcp_keep_alive_policy keep_alive) {
    validate_tcp_no_delay_policy(no_delay);
    validate_tcp_keep_alive_policy(keep_alive);
}

inline void apply_tcp_socket_policies(asio::ip::tcp::socket& socket, tcp_no_delay_policy no_delay,
    tcp_keep_alive_policy keep_alive) noexcept {
    std::error_code ignored;
    if (no_delay == tcp_no_delay_policy::enable) {
        (void)socket.set_option(asio::ip::tcp::no_delay(true), ignored);
    }
    if (keep_alive == tcp_keep_alive_policy::enable) {
        (void)socket.set_option(asio::socket_base::keep_alive(true), ignored);
    }
}

}  // namespace ruvia
