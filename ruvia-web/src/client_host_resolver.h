#pragma once

#include <stdexcept>
#include <string_view>

#include "dns_resolver.h"
#include "startup_config.h"

namespace ruvia::detail {

using ::ruvia::is_valid_dns_host;
using client_resolve_result = ::ruvia::dns_resolve_result;
using client_resolve_operation = ::ruvia::dns_resolve_operation;
using client_host_resolver = ::ruvia::dns_resolver;

[[nodiscard]] inline bool is_client_ip_address(std::string_view host) noexcept {
    return ::ruvia::is_valid_http_ipv4_literal(host) || ::ruvia::is_valid_http_ipv6_literal(host);
}

[[nodiscard]] bool client_host_needs_lookup(std::string_view host);
void require_client_host_lookup(std::string_view host, const worker_ref& worker);

inline void validate_client_origin_host(std::string_view host, const char* empty_message, const char* invalid_message) {
    ensure_config_host(host, empty_message, invalid_message);
    if (!is_client_ip_address(host) && !is_valid_dns_host(host)) {
        throw std::invalid_argument(invalid_message);
    }
}

}  // namespace ruvia::detail
