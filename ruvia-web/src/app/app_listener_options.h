#pragma once

#include <string_view>

#include <asio/ip/address.hpp>

#include "ruvia/web/server_config.h"

namespace ruvia {

namespace detail {

[[nodiscard]] asio::ip::address normalize_listen_address(std::string_view address);
[[nodiscard]] bool has_tls_configuration(const tls_config& config) noexcept;

// listen() owns its tls_config, the only copy of each private-key password the
// application ever holds: credentials are loaded from it and every password
// is cleansed when listen() returns or throws.
class tls_password_cleanser final {
public:
    explicit tls_password_cleanser(tls_config& config) noexcept
        : config_(config) {}
    ~tls_password_cleanser();

    tls_password_cleanser(const tls_password_cleanser&) = delete;
    tls_password_cleanser& operator=(const tls_password_cleanser&) = delete;
    tls_password_cleanser(tls_password_cleanser&&) = delete;
    tls_password_cleanser& operator=(tls_password_cleanser&&) = delete;

private:
    tls_config& config_;
};

// Returns the complete Alt-Svc field value for TCP/TLS responses. An empty
// result disables automatic injection. active_http3_port is absent when this
// listener does not serve HTTP/3.
[[nodiscard]] std::pmr::string normalize_alt_svc_advertisement(const alt_svc_config& config,
    std::optional<std::uint16_t> active_http3_port, std::pmr::memory_resource* resource);

}  // namespace detail
}  // namespace ruvia
