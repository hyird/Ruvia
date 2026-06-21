#pragma once

#include <asio/ip/tcp.hpp>
#include <asio/ssl.hpp>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <memory_resource>
#include <string>
#include <system_error>

namespace ruvia::detail {

inline void closeSocket(asio::ip::tcp::socket& socket) noexcept {
    std::error_code ignored;
    socket.cancel(ignored);
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
}

inline void configureAcceptedSocket(asio::ip::tcp::socket& socket) noexcept {
    std::error_code ignored;
    socket.set_option(asio::ip::tcp::no_delay(true), ignored);
}

inline bool isHttp2AlpnSelected(asio::ssl::stream<asio::ip::tcp::socket&>& tlsStream) noexcept {
    const unsigned char* selected = nullptr;
    unsigned int selectedLength = 0;
    SSL_get0_alpn_selected(tlsStream.native_handle(), &selected, &selectedLength);
    return selectedLength == 2 && selected != nullptr && std::memcmp(selected, "h2", 2) == 0;
}

inline void assignRemoteAddress(std::pmr::string& output, const asio::ip::address& address) {
    if (!address.is_v4()) {
        output = address.to_string();
        return;
    }

    const auto bytes = address.to_v4().to_bytes();
    std::array<char, 15> buffer;
    char* cursor = buffer.data();
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) {
            *cursor++ = '.';
        }
        const auto [ptr, ec] = std::to_chars(cursor, buffer.data() + buffer.size(), bytes[i]);
        if (ec != std::errc{}) {
            output = address.to_string();
            return;
        }
        cursor = ptr;
    }
    output.assign(buffer.data(), static_cast<std::size_t>(cursor - buffer.data()));
}

}  // namespace ruvia::detail
