#pragma once

#include <memory>
#include <memory_resource>
#include <utility>

#include "ruvia/http/detail/util/pmr_resource.h"

namespace ruvia::detail {

template <typename t_type, typename... args_type>
[[nodiscard]] t_type* construct_http_pmr_object(
    http_resolved_pmr_resource_tag, std::pmr::memory_resource* resource, args_type&&... args) {
    auto* storage = resource->allocate(sizeof(t_type), alignof(t_type));
    try {
        return std::construct_at(static_cast<t_type*>(storage), std::forward<args_type>(args)...);
    } catch (...) {
        resource->deallocate(storage, sizeof(t_type), alignof(t_type));
        throw;
    }
}

template <typename t_type, typename... args_type>
[[nodiscard]] t_type* construct_http_pmr_object(std::pmr::memory_resource* resource, args_type&&... args) {
    return construct_http_pmr_object<t_type>(http_resolved_pmr_resource_tag{},
        http_pmr_resource_or_default(resource), std::forward<args_type>(args)...);
}

template <typename t_type>
void destroy_http_pmr_object(
    http_resolved_pmr_resource_tag, t_type* value, std::pmr::memory_resource* resource) noexcept {
    if (value == nullptr) {
        return;
    }
    std::destroy_at(value);
    resource->deallocate(value, sizeof(t_type), alignof(t_type));
}

template <typename t_type>
void destroy_http_pmr_object(t_type* value, std::pmr::memory_resource* resource) noexcept {
    destroy_http_pmr_object(http_resolved_pmr_resource_tag{}, value, http_pmr_resource_or_default(resource));
}

template <typename t_type>
struct http_pmr_object_deleter final {
    std::pmr::memory_resource* resource_{nullptr};

    void operator()(t_type* value) const noexcept {
        destroy_http_pmr_object(value, resource_);
    }
};

template <typename t_type, typename... args_type>
[[nodiscard]] std::unique_ptr<t_type, http_pmr_object_deleter<t_type>> make_http_pmr_object(
    std::pmr::memory_resource* resource, args_type&&... args) {
    auto* memory_resource = http_pmr_resource_or_default(resource);
    return std::unique_ptr<t_type, http_pmr_object_deleter<t_type>>(
        construct_http_pmr_object<t_type>(
            http_resolved_pmr_resource_tag{}, memory_resource, std::forward<args_type>(args)...),
        http_pmr_object_deleter<t_type>{memory_resource});
}

}  // namespace ruvia::detail
