#include "server/peer_address.h"

#include <array>
#include <cstring>

#include <asio/detail/socket_ops.hpp>
#include <asio/error.hpp>

#include "ratelimit/rate_limit_key.h"

namespace ruvia::detail {

void assign_peer_address(std::pmr::string& output, const asio::ip::address& address) {
    asio::ip::address_v4 v4;
    if (address.is_v4()) {
        v4 = address.to_v4();
    } else {
        const auto v6 = address.to_v6();
        if (!v6.is_v4_mapped()) {
            const auto bytes = v6.to_bytes();
            std::array<char, 64> ipv6_buffer;
            asio::error_code ec;
            const char* formatted = asio::detail::socket_ops::inet_ntop(
                ASIO_OS_DEF(AF_INET6), bytes.data(), ipv6_buffer.data(),
                ipv6_buffer.size(), v6.scope_id(), ec);
            if (formatted == nullptr) {
                asio::detail::throw_error(ec);
            }
            output.assign(formatted, std::strlen(formatted));
            return;
        }
        v4 = asio::ip::make_address_v4(asio::ip::v4_mapped, v6);
    }
    std::array<char, 15> buffer;
    const auto formatted = format_ipv4_address(v4, buffer);
    output.assign(formatted.data(), formatted.size());
}

}  // namespace ruvia::detail
