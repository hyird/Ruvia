#pragma once

#include <system_error>

#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/socket_base.hpp>

namespace ruvia::test {

// HTTPS tests also start QUIC on the same port. TCP and UDP have independent
// ephemeral ranges and permissions on Windows, so let UDP choose a port first
// and verify its TCP counterpart. These probes close before returning; the
// application still owns and binds every real listener, without startup retries.
[[nodiscard]] inline asio::ip::tcp::endpoint select_https_endpoint(
    asio::io_context& io, const asio::ip::address& address) {
    std::error_code last_error;
    for (int attempt = 0; attempt < 128; ++attempt) {
        asio::ip::udp::socket udp(io, {address, 0});
        const asio::ip::tcp::endpoint endpoint(address, udp.local_endpoint().port());
        asio::ip::tcp::acceptor tcp(io);
        tcp.open(endpoint.protocol());
        tcp.bind(endpoint, last_error);
        if (!last_error) {
            tcp.listen(asio::socket_base::max_listen_connections, last_error);
        }
        if (!last_error) {
            return endpoint;
        }
        if (last_error != asio::error::address_in_use && last_error != asio::error::access_denied) {
            throw std::system_error(last_error, "cannot select HTTPS test endpoint");
        }
    }
    throw std::system_error(last_error, "no TCP/UDP HTTPS test endpoint available");
}

}  // namespace ruvia::test
