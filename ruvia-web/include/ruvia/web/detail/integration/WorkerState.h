#pragma once

// Worker-local user state: App::useWorkerState<T>() registers one recipe, each
// worker builds its own instance from it, and Context::workerState<T>() /
// WebWorkerContext::workerState<T>() hand the instance back on that worker.
// This generalizes the DbRegistry pattern (one connection-pool-like object per
// single-threaded worker) to application-owned types: the instance is only
// ever touched from its worker's thread, so it needs no synchronization.

#include <algorithm>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/core/memory/PmrResource.h"
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

    [[nodiscard]] bool valid() const noexcept {
        return typeKey_ != nullptr && factory_ != nullptr && destroyFactory_ != nullptr &&
               createInstance_ != nullptr && destroyInstance_ != nullptr;
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

template <typename Definitions>
void appendWorkerStateDefinition(Definitions& definitions, WorkerStateDefinition&& definition) {
    if (!definition.valid()) {
        throw std::invalid_argument("worker state definition is invalid");
    }
    for (const auto& existing : definitions) {
        if (existing.typeKey() == definition.typeKey()) {
            throw std::invalid_argument("worker state type is already registered");
        }
    }
    definitions.push_back(std::move(definition));
}

template <typename Definitions>
void validateWorkerStateDefinitions(const Definitions& definitions) {
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        if (!definitions[index].valid()) {
            throw std::invalid_argument("worker state definition is invalid");
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (definitions[previous].typeKey() == definitions[index].typeKey()) {
                throw std::invalid_argument("worker state type is already registered");
            }
        }
    }
}

// The per-worker instances. WebWorkerRuntime explicitly initializes and
// destroys the registry inside its active worker identity window; a throwing
// factory fails startup before the worker dispatches callbacks or requests.
// Registration order remains the lifetime order (destruction is reversed),
// while a separate immutable type index gives request-time lookup one
// allocation-free binary search.
class WorkerStateRegistry final {
public:
    WorkerStateRegistry(
        std::pmr::memory_resource* resource, std::span<const WorkerStateDefinition> definitions)
        : resource_(validatedResource(resource, definitions)),
          definitions_(definitions),
          entries_(resource_),
          typeIndex_(resource_) {}

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
        validateWorkerStateDefinitions(definitions_);
        entries_.reserve(definitions_.size());
        typeIndex_.reserve(definitions_.size());
        try {
            for (const auto& definition : definitions_) {
                entries_.push_back(InstanceEntry{definition.typeKey_,
                    definition.createInstance_(definition.factory_, resource_),
                    definition.destroyInstance_});
                typeIndex_.push_back(TypeIndexEntry{definition.typeKey_, entries_.size() - 1});
            }
            std::ranges::sort(typeIndex_, std::less<const void*>{}, &TypeIndexEntry::typeKey);
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
        const auto match = std::ranges::lower_bound(
            typeIndex_, typeKey, std::less<const void*>{}, &TypeIndexEntry::typeKey);
        if (match == typeIndex_.end() || match->typeKey != typeKey) {
            return nullptr;
        }
        return entries_[match->entryIndex].instance;
    }

private:
    // Called from the first member initializer so malformed startup input cannot
    // allocate container bookkeeping on standard libraries with debug proxies.
    [[nodiscard]] static std::pmr::memory_resource* validatedResource(
        std::pmr::memory_resource* resource, std::span<const WorkerStateDefinition> definitions) {
        validateWorkerStateDefinitions(definitions);
        return pmrResourceOrDefault(resource);
    }

    struct InstanceEntry final {
        const void* typeKey{nullptr};
        void* instance{nullptr};
        WorkerStateDefinition::DestroyInstance destroy{nullptr};
    };

    struct TypeIndexEntry final {
        const void* typeKey{nullptr};
        std::size_t entryIndex{0};
    };

    void destroyEntries() noexcept {
        typeIndex_.clear();
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
    std::pmr::vector<InstanceEntry> entries_;
    std::pmr::vector<TypeIndexEntry> typeIndex_;
    bool initialized_{false};
};

}  // namespace ruvia::detail
