#pragma once

// Worker-local user state: App::useWorkerState<T>() registers one recipe, each
// worker builds its own instance from it, and Context::workerState<T>() /
// WebWorkerContext::workerState<T>() hand the instance back on that worker.
// This generalizes the DbRegistry pattern (one connection-pool-like object per
// single-threaded worker) to application-owned types: the instance is only
// ever touched from its worker's thread, so it needs no synchronization.

#include <cstddef>
#include <exception>
#include <memory_resource>
#include <span>
#include <stdexcept>
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
// factory plus the instance create/destroy pair. The factory runs once inside
// each worker's active identity window before that worker begins dispatching
// callbacks or requests.
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
        static_assert(std::is_invocable_v<Stored&>,
            "worker state factory must be invocable with no arguments");
        static_assert(std::is_constructible_v<T, std::invoke_result_t<Stored&>>,
            "worker state factory must return a value that constructs T");
        WorkerStateDefinition definition;
        definition.typeKey_ = workerStateTypeKey<T>();
        definition.factory_ =
            constructPmrObject<Stored>(processResource(), std::forward<Factory>(factory));
        // Named apart from the enclosing `factory` parameter: these lambdas are
        // captureless and receive the erased pointer, not that object.
        definition.destroyFactory_ = [](void* storedFactory) noexcept {
            destroyPmrObject(static_cast<Stored*>(storedFactory), processResource());
        };
        definition.createInstance_ = [](void* storedFactory,
                                         std::pmr::memory_resource* resource) -> void* {
            return constructPmrObject<T>(resource, (*static_cast<Stored*>(storedFactory))());
        };
        definition.destroyInstance_ = [](void* instance,
                                          std::pmr::memory_resource* resource) noexcept {
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

// The per-worker instances. WebWorkerRuntime explicitly initializes and
// destroys the registry inside its active worker identity window; a throwing
// factory fails startup before the worker dispatches callbacks or requests.
// Lookup is a linear scan: the set is small, fixed after initialization, and
// read-only afterwards.
class WorkerStateRegistry final {
public:
    WorkerStateRegistry(
        std::pmr::memory_resource* resource, std::span<const WorkerStateDefinition> definitions)
        : resource_(resource),
          definitions_(definitions),
          entries_(resource) {}

    WorkerStateRegistry(const WorkerStateRegistry&) = delete;
    WorkerStateRegistry& operator=(const WorkerStateRegistry&) = delete;
    WorkerStateRegistry(WorkerStateRegistry&&) = delete;
    WorkerStateRegistry& operator=(WorkerStateRegistry&&) = delete;

    ~WorkerStateRegistry() {
        if (initialized_) {
            std::terminate();
        }
    }

    void initialize() {
        if (initialized_) {
            throw std::logic_error("worker state registry is already initialized");
        }
        entries_.reserve(definitions_.size());
        try {
            for (const auto& definition : definitions_) {
                entries_.push_back(Entry{definition.typeKey_,
                    definition.createInstance_(definition.factory_, resource_),
                    definition.destroyInstance_});
            }
        } catch (...) {
            destroyEntries();
            throw;
        }
        initialized_ = true;
    }

    void shutdown() noexcept {
        if (!initialized_) {
            return;
        }
        destroyEntries();
        initialized_ = false;
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
    std::span<const WorkerStateDefinition> definitions_;
    std::pmr::vector<Entry> entries_;
    bool initialized_{false};
};

}  // namespace ruvia::detail
