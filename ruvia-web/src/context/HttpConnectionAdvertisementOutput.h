#pragma once

#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/ScopedOperation.h"

namespace ruvia::detail {

// Request-scoped typed access to the owning connection's advertisement lane.
// Repeated operation storage belongs to the worker pool, not the request arena.
class HttpConnectionAdvertisementOutput final {
public:
    using Origins = Task<void> (*)(void*, std::span<const std::string_view>);
    using AlternativeService = Task<void> (*)(void*, std::string_view);
    HttpConnectionAdvertisementOutput(std::pmr::memory_resource* resource, void* target,
        Origins origins, AlternativeService service) noexcept
        : resource_(resource),
          target_(target),
          origins_(origins),
          service_(service) {}
    [[nodiscard]] ScopedOperation<void> advertiseOrigins(std::span<const std::string_view> origins);
    [[nodiscard]] ScopedOperation<void> advertiseAlternativeService(std::string_view value);

private:
    [[nodiscard]] Task<void> writeOrigins(std::pmr::vector<std::pmr::string> origins);
    [[nodiscard]] Task<void> writeService(std::pmr::string value);
    std::pmr::memory_resource* resource_;
    void* target_;
    Origins origins_;
    AlternativeService service_;
    ::ruvia::operation_scope scope_;
};

}  // namespace ruvia::detail
