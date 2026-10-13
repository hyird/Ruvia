#pragma once

#include <stdexcept>
#include <string_view>

#include "util/dns_host.h"

namespace ruvia::detail {

[[nodiscard]] inline bool is_valid_sni_host(std::string_view host) noexcept {
    return !host.empty() && !host.ends_with('.') && detail::is_valid_dns_host(host);
}

inline void ensure_sni_host(std::string_view host) {
    if (host.empty()) {
        throw std::invalid_argument("SNI host must not be empty");
    }
    if (!is_valid_sni_host(host)) {
        throw std::invalid_argument("SNI host is invalid");
    }
}

}  // namespace ruvia::detail
