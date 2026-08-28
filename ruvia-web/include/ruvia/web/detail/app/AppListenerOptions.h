#pragma once

#include <memory_resource>
#include <string_view>

#include <asio/ip/address.hpp>

#include "ruvia/web/ServerConfig.h"
#include "ruvia/web/detail/server/HttpServerListener.h"

namespace ruvia {

namespace detail {

[[nodiscard]] asio::ip::address normalizeListenAddress(std::string_view address);
[[nodiscard]] bool hasTlsConfiguration(const TlsConfig& config) noexcept;

// TLS paths reach the server as PMR strings, so a filesystem path whose native
// encoding is not char (Windows) is converted and the complete normalized
// configuration is validated here exactly once.
[[nodiscard]] HttpServerListenerDefinition::Tls normalizeTlsOptions(const TlsConfig& config, std::pmr::memory_resource* resource);

}  // namespace detail
}  // namespace ruvia
