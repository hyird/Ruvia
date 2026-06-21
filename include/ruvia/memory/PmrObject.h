#pragma once

#include <memory>
#include <memory_resource>
#include <utility>

#include "ruvia/memory/PmrResource.h"

namespace ruvia::detail {

template <typename T, typename... Args>
[[nodiscard]] T* constructPmrObject(std::pmr::memory_resource* resource, Args&&... args) {
    auto* memoryResource = pmrResourceOrDefault(resource);
    auto* storage = memoryResource->allocate(sizeof(T), alignof(T));
    try {
        return std::construct_at(static_cast<T*>(storage), std::forward<Args>(args)...);
    } catch (...) {
        memoryResource->deallocate(storage, sizeof(T), alignof(T));
        throw;
    }
}

template <typename T>
void destroyPmrObject(T* value, std::pmr::memory_resource* resource) noexcept {
    if (value == nullptr) {
        return;
    }
    auto* memoryResource = pmrResourceOrDefault(resource);
    std::destroy_at(value);
    memoryResource->deallocate(value, sizeof(T), alignof(T));
}

template <typename T>
struct PmrObjectDeleter final {
    std::pmr::memory_resource* resource{std::pmr::get_default_resource()};

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
        constructPmrObject<T>(memoryResource, std::forward<Args>(args)...),
        PmrObjectDeleter<T>{memoryResource});
}

}  // namespace ruvia::detail
