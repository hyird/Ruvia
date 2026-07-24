#pragma once

#include <memory_resource>

namespace ruvia::detail {

struct HttpResolvedPmrResourceTag final {};

// The core target has an independent runtime memory-resource adapter; HTTP keeps
// this protocol-local version so the standalone target never depends on core.
// (pmrResourceOrDefault + ResolvedPmrResourceTag). The two are NOT redundant and
// must NOT be merged: this http version falls back to std::pmr::get_default_
// resource() because ruvia-http must stay core-independent, while the core
// version falls back to processResource(). Keep this for HTTP-internal use only;
// Web/Core callers use the Core adapter.
[[nodiscard]] inline std::pmr::memory_resource* httpPmrResourceOrDefault(std::pmr::memory_resource* resource) noexcept {
    return resource == nullptr ? std::pmr::get_default_resource() : resource;
}

}  // namespace ruvia::detail
