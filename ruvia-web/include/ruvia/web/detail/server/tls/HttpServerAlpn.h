#pragma once

#include <cstring>

#include <asio/ip/tcp.hpp>
#include <asio/ssl.hpp>

namespace ruvia::detail {

inline bool isHttp2AlpnSelected(asio::ssl::stream<asio::ip::tcp::socket&>& tlsStream) noexcept {
    const unsigned char* selected = nullptr;
    unsigned int selectedLength = 0;
    SSL_get0_alpn_selected(tlsStream.native_handle(), &selected, &selectedLength);
    return selectedLength == 2 && selected != nullptr && std::memcmp(selected, "h2", 2) == 0;
}

}  // namespace ruvia::detail
