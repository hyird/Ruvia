#pragma once

#include "ruvia/http/detail/util/PmrResource.h"

#include <memory>
#include <memory_resource>
#include <utility>

namespace ruvia::detail {

template <typename T, typename... Args>
[[nodiscard]] T* constructHttpPmrObject(
    HttpResolvedPmrResourceTag, std::pmr::memory_resource* resource, Args&&... args) {
    auto* storage = resource->allocate(sizeof(T), alignof(T));
    try {
        return std::construct_at(static_cast<T*>(storage), std::forward<Args>(args)...);
    } catch (...) {
        resource->deallocate(storage, sizeof(T), alignof(T));
        throw;
    }
}

template <typename T, typename... Args>
[[nodiscard]] T* constructHttpPmrObject(std::pmr::memory_resource* resource, Args&&... args) {
    return constructHttpPmrObject<T>(HttpResolvedPmrResourceTag{},
        httpPmrResourceOrDefault(resource), std::forward<Args>(args)...);
}

template <typename T>
void destroyHttpPmrObject(
    HttpResolvedPmrResourceTag, T* value, std::pmr::memory_resource* resource) noexcept {
    if (value == nullptr) {
        return;
    }
    std::destroy_at(value);
    resource->deallocate(value, sizeof(T), alignof(T));
}

template <typename T>
void destroyHttpPmrObject(T* value, std::pmr::memory_resource* resource) noexcept {
    destroyHttpPmrObject(HttpResolvedPmrResourceTag{}, value, httpPmrResourceOrDefault(resource));
}

template <typename T>
struct HttpPmrObjectDeleter final {
    std::pmr::memory_resource* resource{nullptr};

    void operator()(T* value) const noexcept {
        destroyHttpPmrObject(value, resource);
    }
};

template <typename T, typename... Args>
[[nodiscard]] std::unique_ptr<T, HttpPmrObjectDeleter<T>> makeHttpPmrObject(
    std::pmr::memory_resource* resource, Args&&... args) {
    auto* memoryResource = httpPmrResourceOrDefault(resource);
    return std::unique_ptr<T, HttpPmrObjectDeleter<T>>(
        constructHttpPmrObject<T>(
            HttpResolvedPmrResourceTag{}, memoryResource, std::forward<Args>(args)...),
        HttpPmrObjectDeleter<T>{memoryResource});
}

}  // namespace ruvia::detail
