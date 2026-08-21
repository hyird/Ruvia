#pragma once

#include "ruvia/web/ServerConfig.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"

namespace ruvia {

class StaticRoot;

namespace detail {

// Translating an App's configuration into the options one listener runs with.
// A listener differs from the app-wide base only in its transport and the
// document root it serves, so the already-normalized base is copied and only
// the listener transport is replaced.
[[nodiscard]] HttpServerOptions makeListenerOptions(const HttpServerOptions& base, HttpServerOptions::ListenerTransport transport);

// TLS paths reach the server as PMR strings, so a filesystem path whose native
// encoding is not char (Windows) is converted here rather than at every use.
[[nodiscard]] HttpServerOptions::Tls makeTlsOptions(const TlsConfig& config, std::pmr::memory_resource* resource);

}  // namespace detail
}  // namespace ruvia
