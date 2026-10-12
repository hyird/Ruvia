#pragma once

#include <stdexcept>
#include <string_view>

#include "ruvia/http/http_request_target.h"

#include "util/config_validation.h"
#include "util/dns_host.h"

namespace ruvia::detail {

[[nodiscard]] inline bool is_client_ip_address(std::string_view host) noexcept {
    return ::ruvia::is_valid_http_ipv4_literal(host) || ::ruvia::is_valid_http_ipv6_literal(host);
}

// A client transport host, also used for TLS SNI and peer verification: an
// unbracketed IP literal or an ASCII DNS host name.
inline void validate_client_origin_host(
    std::string_view host, const char* empty_message, const char* invalid_message) {
    ensure_config_host(host, empty_message, invalid_message, separated_port_host_rules);
    if (!is_client_ip_address(host) && !is_valid_dns_host(host)) {
        throw std::invalid_argument(invalid_message);
    }
}

}  // namespace ruvia::detail
