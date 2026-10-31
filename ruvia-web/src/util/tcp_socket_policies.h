#pragma once

#include <cstdint>
#include <system_error>

#include <asio/ip/tcp.hpp>

namespace ruvia::detail {

// Whether a connection asks the kernel to probe an idle peer. HTTP and
// WebSocket clients do; Redis connections and accepted server sockets keep the
// system default.
enum class tcp_keep_alive : std::uint8_t {
    system_default,
    enable,
};

// Exchanges are latency bound, so every connection disables Nagle's
// algorithm. Returns the error of the first policy that cannot be applied.
[[nodiscard]] inline std::error_code apply_tcp_socket_policies(
    asio::ip::tcp::socket& socket, tcp_keep_alive keep_alive) noexcept {
    std::error_code error;
    socket.set_option(asio::ip::tcp::no_delay(true), error);
    if (!error && keep_alive == tcp_keep_alive::enable) {
        socket.set_option(asio::socket_base::keep_alive(true), error);
    }
    return error;
}

}  // namespace ruvia::detail
