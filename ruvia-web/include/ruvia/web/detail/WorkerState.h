#pragma once

// Worker-local user state: App::useWorkerState<T>() registers one recipe, each
// worker builds its own instance from it, and Context::workerState<T>() /
// WebWorkerContext::workerState<T>() hand the instance back on that worker.
// This generalizes the DbRegistry pattern (one connection-pool-like object per
// single-threaded worker) to application-owned types: the instance is only
// ever touched from its worker's thread, so it needs no synchronization.

#include <cstddef>
#include <memory_resource>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/core/memory/ProcessResource.h"

namespace ruvia::detail {

// One inline tag per T gives a process-wide unique key that links the App
// registration to the Context accessor across translation units.
template <typename T>
inline constexpr char workerStateTypeTag = 0;

template <typename T>
[[nodiscard]] const void* workerStateTypeKey() noexcept {
    return &workerStateTypeTag<T>;
}

// Startup-registered recipe for one per-worker state type: the erased user
// factory plus the instance create/destroy pair. The factory runs once per
// worker; workers are constructed sequentially on the startup thread, so a
// stateful factory needs no synchronization either.
class WorkerStateDefinition final {
public:
    WorkerStateDefinition(const WorkerStateDefinition&) = delete;
    WorkerStateDefinition& operator=(const WorkerStateDefinition&) = delete;

    WorkerStateDefinition(WorkerStateDefinition&& other) noexcept
        : typeKey_(std::exchange(other.typeKey_, nullptr)),
          factory_(std::exchange(other.factory_, nullptr)),
          destroyFactory_(std::exchange(other.destroyFactory_, nullptr)),
          createInstance_(std::exchange(other.createInstance_, nullptr)),
          destroyInstance_(std::exchange(other.destroyInstance_, nullptr)) {}
    WorkerStateDefinition& operator=(WorkerStateDefinition&&) = delete;

    ~WorkerStateDefinition() {
        if (factory_ != nullptr && destroyFactory_ != nullptr) {
            destroyFactory_(factory_);
        }
    }

    template <typename T, typename Factory>
    [[nodiscard]] static WorkerStateDefinition make(Factory&& factory) {
        using Stored = std::decay_t<Factory>;
        static_assert(
            std::is_invocable_v<Stored&>,
            "worker state factory must be invocable with no arguments");
        static_assert(
            std::is_constructible_v<T, std::invoke_result_t<Stored&>>,
            "worker state factory must return a value that constructs T");
        WorkerStateDefinition definition;
        definition.typeKey_ = workerStateTypeKey<T>();
        definition.factory_ =
            constructPmrObject<Stored>(processResource(), std::forward<Factory>(factory));
        definition.destroyFactory_ = [](void* factory) noexcept {
            destroyPmrObject(static_cast<Stored*>(factory), processResource());
        };
        definition.createInstance_ =
            [](void* factory, std::pmr::memory_resource* resource) -> void* {
            return constructPmrObject<T>(
                resource, (*static_cast<Stored*>(factory))());
        };
        definition.destroyInstance_ =
            [](void* instance, std::pmr::memory_resource* resource) noexcept {
            destroyPmrObject(static_cast<T*>(instance), resource);
        };
        return definition;
    }

    [[nodiscard]] const void* typeKey() const noexcept {
        return typeKey_;
    }

private:
    friend class WorkerStateRegistry;

    using DestroyFactory = void (*)(void*) noexcept;
    using CreateInstance = void* (*)(void*, std::pmr::memory_resource*);
    using DestroyInstance = void (*)(void*, std::pmr::memory_resource*) noexcept;

    WorkerStateDefinition() noexcept = default;

    const void* typeKey_{nullptr};
    void* factory_{nullptr};
    DestroyFactory destroyFactory_{nullptr};
    CreateInstance createInstance_{nullptr};
    DestroyInstance destroyInstance_{nullptr};
};

// The per-worker instances, built eagerly at worker construction so a throwing
// factory fails startup instead of the first request. Lookup is a linear scan:
// the set is small, fixed after construction, and read-only afterwards.
class WorkerStateRegistry final {
public:
    WorkerStateRegistry(
        std::pmr::memory_resource* resource,
        std::span<const WorkerStateDefinition> definitions)
        : resource_(resource), entries_(resource) {
        entries_.reserve(definitions.size());
        try {
            for (const auto& definition : definitions) {
                entries_.push_back(Entry{
                    definition.typeKey_,
                    definition.createInstance_(definition.factory_, resource_),
                    definition.destroyInstance_});
            }
        } catch (...) {
            destroyEntries();
            throw;
        }
    }

    WorkerStateRegistry(const WorkerStateRegistry&) = delete;
    WorkerStateRegistry& operator=(const WorkerStateRegistry&) = delete;
    WorkerStateRegistry(WorkerStateRegistry&&) = delete;
    WorkerStateRegistry& operator=(WorkerStateRegistry&&) = delete;

    ~WorkerStateRegistry() {
        destroyEntries();
    }

    [[nodiscard]] void* instance(const void* typeKey) const noexcept {
        for (const auto& entry : entries_) {
            if (entry.typeKey == typeKey) {
                return entry.instance;
            }
        }
        return nullptr;
    }

private:
    struct Entry final {
        const void* typeKey{nullptr};
        void* instance{nullptr};
        WorkerStateDefinition::DestroyInstance destroy{nullptr};
    };

    void destroyEntries() noexcept {
        for (std::size_t i = entries_.size(); i > 0; --i) {
            auto& entry = entries_[i - 1];
            if (entry.instance != nullptr && entry.destroy != nullptr) {
                entry.destroy(entry.instance, resource_);
            }
        }
        entries_.clear();
    }

    std::pmr::memory_resource* resource_;
    std::pmr::vector<Entry> entries_;
};

}  // namespace ruvia::detail
