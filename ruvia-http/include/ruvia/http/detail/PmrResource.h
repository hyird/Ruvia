#pragma once

#include <memory_resource>

namespace ruvia::detail {

struct HttpResolvedPmrResourceTag final {};

// A parallel version lives in ruvia-core/include/ruvia/memory/PmrResource.h
// (pmrResourceOrDefault + ResolvedPmrResourceTag). The two are NOT redundant and
// must NOT be merged: this http version falls back to std::pmr::get_default_
// resource() because ruvia-http must stay core-independent, while the core
// version falls back to processResource() (the mimalloc process pool). Keep this
// for http-internal use only; web/core callers use the core version so a null
// resource lands in the mimalloc pool. Introduced in d947416.
[[nodiscard]] inline std::pmr::memory_resource* httpPmrResourceOrDefault(
    std::pmr::memory_resource* resource) noexcept {
    return resource == nullptr ? std::pmr::get_default_resource() : resource;
}

}  // namespace ruvia::detail
