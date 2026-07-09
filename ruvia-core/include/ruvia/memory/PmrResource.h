#pragma once

#include <memory_resource>

#include "ruvia/memory/ProcessResource.h"

namespace ruvia::detail {

struct ResolvedPmrResourceTag final {};

// A parallel http-only version lives in ruvia-http/include/ruvia/http/detail/
// PmrResource.h (httpPmrResourceOrDefault + HttpResolvedPmrResourceTag). The
// two are NOT redundant and must NOT be merged: this core version falls back to
// processResource() (the mimalloc process pool), while the http version -- which
// cannot depend on core -- deliberately falls back to std::pmr::get_default_
// resource() (plain new/delete). web deliberately uses THIS core version so a
// null request resource still lands in the mimalloc pool instead of the stdlib
// allocator. Introduced in d947416.
[[nodiscard]] inline std::pmr::memory_resource* pmrResourceOrDefault(
    std::pmr::memory_resource* resource) noexcept {
    return resource == nullptr ? processResource() : resource;
}

}  // namespace ruvia::detail
