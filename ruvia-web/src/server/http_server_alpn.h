#pragma once

#include <cstring>

#include "server/server_tls_stream.h"

namespace ruvia::detail {

inline bool is_http2_alpn_selected(server_tls_stream& tls_stream) noexcept {
    const unsigned char* selected = nullptr;
    unsigned int selected_length = 0;
    SSL_get0_alpn_selected(tls_stream.native_handle(), &selected, &selected_length);
    return selected_length == 2 && selected != nullptr && std::memcmp(selected, "h2", 2) == 0;
}

}  // namespace ruvia::detail
