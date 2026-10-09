#pragma once

#include <span>
#include <string_view>

#include "ruvia/http/http_header.h"

namespace ruvia {
// Borrowed only until open_tunnel() returns its cold operation. Normal CONNECT
// uses authority-form and leaves protocol/target empty. Extended CONNECT uses
// protocol, authority and an origin-form target on HTTP/2 or HTTP/3.
struct http_client_tunnel_request_view final {
    std::string_view authority_{};
    std::string_view protocol_{};
    std::string_view target_{};
    std::span<const http_header_view> headers_{};
};
}  // namespace ruvia
