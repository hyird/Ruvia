#pragma once

#include <stdexcept>
#include <string_view>

namespace ruvia::detail {

// One canonical spelling is shared by App, TestApp and RouteTable. RouterImpl
// receives the already-normalized registrations from the public configuration
// facades and keeps the final table's validation as its defensive boundary.
// A trailing slash does not create a different path scope; accepting both
// spellings at one layer and not another makes duplicate fallback handlers
// order-dependent and lets tests exercise a different route graph.
[[nodiscard]] inline std::string_view normalizeFallbackPrefix(std::string_view prefix) {
    if (prefix.empty() || prefix.front() != '/') {
        throw std::invalid_argument("fallback prefix must start with '/'");
    }
    while (prefix.size() > 1 && prefix.back() == '/') {
        prefix.remove_suffix(1);
    }
    return prefix;
}

}  // namespace ruvia::detail
