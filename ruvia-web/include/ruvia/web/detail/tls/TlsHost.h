#pragma once

#include <stdexcept>
#include <string_view>

#include "ruvia/core/detail/util/DnsHost.h"

namespace ruvia::detail {

[[nodiscard]] inline bool isValidSniHost(std::string_view host) noexcept {
    return !host.empty() && !host.ends_with('.') && isValidDnsHost(host);
}

inline void ensureSniHost(
    std::string_view host, const char* emptyMessage, const char* invalidMessage) {
    if (host.empty()) {
        throw std::invalid_argument(emptyMessage);
    }
    if (!isValidSniHost(host)) {
        throw std::invalid_argument(invalidMessage);
    }
}

}  // namespace ruvia::detail
