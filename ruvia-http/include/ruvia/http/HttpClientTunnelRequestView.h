#pragma once

#include <span>
#include <string_view>

#include "ruvia/http/HttpHeader.h"

namespace ruvia {
// Borrowed only until openTunnel() returns its cold operation. Normal CONNECT
// uses authority-form and leaves protocol/target empty. Extended CONNECT uses
// protocol, authority and an origin-form target on HTTP/2 or HTTP/3.
struct HttpClientTunnelRequestView final {
    std::string_view authority{};
    std::string_view protocol{};
    std::string_view target{};
    std::span<const HttpHeaderView> headers{};
};
}  // namespace ruvia
