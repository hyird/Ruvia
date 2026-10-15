#pragma once

#include <cstring>

#include <asio/ip/tcp.hpp>
#include <asio/ssl.hpp>

namespace ruvia::detail {

inline bool is_http2_alpn_selected(asio::ssl::stream<asio::ip::tcp::socket&>& tls_stream) noexcept {
    const unsigned char* selected = nullptr;
    unsigned int selected_length = 0;
    SSL_get0_alpn_selected(tls_stream.native_handle(), &selected, &selected_length);
    return selected_length == 2 && selected != nullptr && std::memcmp(selected, "h2", 2) == 0;
}

}  // namespace ruvia::detail
