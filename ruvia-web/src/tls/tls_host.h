#pragma once

#include <stdexcept>
#include <string_view>

#include "util/dns_host.h"

namespace ruvia::detail {

[[nodiscard]] inline bool is_valid_sni_host(std::string_view host) noexcept {
    return !host.empty() && !host.ends_with('.') && detail::is_valid_dns_host(host);
}

inline void ensure_sni_host(
    std::string_view host, const char* empty_message, const char* invalid_message) {
    if (host.empty()) {
        throw std::invalid_argument(empty_message);
    }
    if (!is_valid_sni_host(host)) {
        throw std::invalid_argument(invalid_message);
    }
}

}  // namespace ruvia::detail
