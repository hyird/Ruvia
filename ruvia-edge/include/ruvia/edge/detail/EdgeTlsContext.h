#pragma once

#include <memory>

#include <asio/ssl.hpp>

#include "ruvia/edge/EdgeTypes.h"

namespace ruvia::edge {

// Build a server TLS context from PEM, advertising ALPN in server-preference
// order (h2, then http/1.1). Throws asio::system_error on invalid PEM.
[[nodiscard]] std::shared_ptr<asio::ssl::context> buildServerTlsContext(
    const EdgeTlsConfig& config);

}  // namespace ruvia::edge
