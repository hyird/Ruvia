#pragma once

#include "ruvia/web/ServerConfig.h"
#include "ruvia/web/detail/server/HttpServerListener.h"

namespace ruvia {

namespace detail {

// TLS paths reach the server as PMR strings, so a filesystem path whose native
// encoding is not char (Windows) is converted here rather than at every use.
[[nodiscard]] HttpServerListenerDefinition::Tls makeTlsOptions(
    const TlsConfig& config, std::pmr::memory_resource* resource);

}  // namespace detail
}  // namespace ruvia
