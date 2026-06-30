#pragma once

#include <memory>
#include <memory_resource>
#include <utility>

#include "ruvia/memory/PmrResource.h"

namespace ruvia::detail {

template <typename T, typename... Args>
[[nodiscard]] T* constructPmrObject(
    ResolvedPmrResourceTag,
    std::pmr::memory_resource* resource,
    Args&&... args) {
    auto* storage = resource->allocate(sizeof(T), alignof(T));
    try {
        return std::construct_at(static_cast<T*>(storage), std::forward<Args>(args)...);
    } catch (...) {
        resource->deallocate(storage, sizeof(T), alignof(T));
        throw;
    }
}

template <typename T, typename... Args>
[[nodiscard]] T* constructPmrObject(std::pmr::memory_resource* resource, Args&&... args) {
    return constructPmrObject<T>(
        ResolvedPmrResourceTag{},
        pmrResourceOrDefault(resource),
        std::forward<Args>(args)...);
}

template <typename T>
void destroyPmrObject(
    ResolvedPmrResourceTag,
    T* value,
    std::pmr::memory_resource* resource) noexcept {
    if (value == nullptr) {
        return;
    }
    std::destroy_at(value);
    resource->deallocate(value, sizeof(T), alignof(T));
}

template <typename T>
void destroyPmrObject(T* value, std::pmr::memory_resource* resource) noexcept {
    destroyPmrObject(ResolvedPmrResourceTag{}, value, pmrResourceOrDefault(resource));
}

template <typename T>
struct PmrObjectDeleter final {
    std::pmr::memory_resource* resource{nullptr};

    void operator()(T* value) const noexcept {
        destroyPmrObject(value, resource);
    }
};

template <typename T, typename... Args>
[[nodiscard]] std::unique_ptr<T, PmrObjectDeleter<T>> makePmrObject(
    std::pmr::memory_resource* resource,
    Args&&... args) {
    auto* memoryResource = pmrResourceOrDefault(resource);
    return std::unique_ptr<T, PmrObjectDeleter<T>>(
        constructPmrObject<T>(
            ResolvedPmrResourceTag{},
            memoryResource,
            std::forward<Args>(args)...),
        PmrObjectDeleter<T>{memoryResource});
}

}  // namespace ruvia::detail
