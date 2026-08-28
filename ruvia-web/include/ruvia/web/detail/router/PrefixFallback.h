#pragma once

#include "ruvia/http/detail/parser/HttpRequestTarget.h"

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
    if (prefix.find('?') != std::string_view::npos || !isValidOriginFormTarget(prefix)) {
        throw std::invalid_argument("fallback prefix must be an origin-form path without query");
    }
    while (prefix.size() > 1 && prefix.back() == '/') {
        prefix.remove_suffix(1);
    }
    return prefix;
}

// The one prefix-scoping rule, shared by fallback handler selection and by
// path-scoped middleware. A prefix matches on whole path segments only, so
// "/api" scopes "/api" and "/api/x" but never "/apix"; "/" scopes everything.
[[nodiscard]] inline bool pathIsUnderPrefix(std::string_view path, std::string_view prefix) noexcept {
    if (prefix.empty() || prefix == "/") {
        return true;
    }
    return path == prefix || (path.size() > prefix.size() && path.starts_with(prefix) && path[prefix.size()] == '/');
}

}  // namespace ruvia::detail
