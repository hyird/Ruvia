#pragma once

#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/scoped_operation.h"

namespace ruvia::detail {

// Request-scoped typed access to the owning connection's advertisement lane.
// Repeated operation storage belongs to the worker pool, not the request arena.
class http_connection_advertisement_output final {
public:
    using origins_type = task<void> (*)(void*, std::span<const std::string_view>);
    using alternative_service_type = task<void> (*)(void*, std::string_view);
    http_connection_advertisement_output(std::pmr::memory_resource* resource, void* target,
        origins_type origins, alternative_service_type service) noexcept
        : resource_(resource),
          target_(target),
          origins_(origins),
          service_(service) {}
    [[nodiscard]] scoped_operation<void> advertise_origins(std::span<const std::string_view> origins);
    [[nodiscard]] scoped_operation<void> advertise_alternative_service(std::string_view value);

private:
    [[nodiscard]] task<void> write_origins(std::pmr::vector<std::pmr::string> origins);
    [[nodiscard]] task<void> write_service(std::pmr::string value);
    std::pmr::memory_resource* resource_;
    void* target_;
    origins_type origins_;
    alternative_service_type service_;
    ::ruvia::operation_scope scope_;
};

}  // namespace ruvia::detail
