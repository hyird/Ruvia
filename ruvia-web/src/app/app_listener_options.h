#pragma once

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include <asio/ip/address.hpp>

#include "ruvia/web/server_config.h"

#include "server/http_server_listener.h"

namespace ruvia {

namespace detail {

[[nodiscard]] asio::ip::address normalize_listen_address(std::string_view address);
[[nodiscard]] bool has_tls_configuration(const tls_config& config) noexcept;

// TLS paths reach the server as PMR strings, so a filesystem path whose native
// encoding is not char (Windows) is converted and the complete normalized
// configuration is validated here exactly once.
[[nodiscard]] http_server_listener_definition::tls_type normalize_tls_options(
    const tls_config& config, std::pmr::memory_resource* resource);

// Returns the complete Alt-Svc field value for TCP/TLS responses. An empty
// result disables automatic injection. active_http3_port is absent when this
// listener does not serve HTTP/3.
[[nodiscard]] std::pmr::string normalize_alt_svc_advertisement(const alt_svc_config& config,
    std::optional<std::uint16_t> active_http3_port, std::pmr::memory_resource* resource);

}  // namespace detail
}  // namespace ruvia
