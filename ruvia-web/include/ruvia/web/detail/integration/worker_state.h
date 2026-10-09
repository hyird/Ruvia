#pragma once

// Worker-local user state: application::use_worker_state<T>() registers one recipe, each
// worker builds its own instance from it, and context::worker_state<T>() /
// web_worker_context::worker_state<T>() hand the instance back on that worker.
// This generalizes the db_registry pattern (one connection-pool-like object per
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

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/core/memory/process_resource.h"
#include "ruvia/web/detail/integration/worker_state_key.h"

namespace ruvia::detail {

// Startup-registered recipe for one per-worker state type: the erased user
// factory plus the instance create/destroy pair. The factory runs once inside
// each worker's active identity window before that worker begins dispatching
// callbacks or requests.
class worker_state_definition final {
public:
    worker_state_definition(const worker_state_definition&) = delete;
    worker_state_definition& operator=(const worker_state_definition&) = delete;

    worker_state_definition(worker_state_definition&& other) noexcept
        : type_key_(std::exchange(other.type_key_, nullptr)),
          factory_(std::exchange(other.factory_, nullptr)),
          destroy_factory_(std::exchange(other.destroy_factory_, nullptr)),
          create_instance_(std::exchange(other.create_instance_, nullptr)),
          destroy_instance_(std::exchange(other.destroy_instance_, nullptr)) {}
    worker_state_definition& operator=(worker_state_definition&&) = delete;

    ~worker_state_definition() {
        if (factory_ != nullptr && destroy_factory_ != nullptr) {
            destroy_factory_(factory_);
        }
    }

    template <typename t_type, typename factory_type>
    [[nodiscard]] static worker_state_definition make(factory_type&& factory) {
        using stored_type = std::decay_t<factory_type>;
        static_assert(std::is_invocable_v<stored_type&>,
            "worker state factory must be invocable with no arguments");
        static_assert(std::is_constructible_v<t_type, std::invoke_result_t<stored_type&>>,
            "worker state factory must return a value that constructs T");
        worker_state_definition definition;
        definition.type_key_ = worker_state_type_key<t_type>();
        definition.factory_ =
            construct_pmr_object<stored_type>(process_resource(), std::forward<factory_type>(factory));
        // Named apart from the enclosing `factory` parameter: these lambdas are
        // captureless and receive the erased pointer, not that object.
        definition.destroy_factory_ = [](void* stored_factory) noexcept {
            destroy_pmr_object(static_cast<stored_type*>(stored_factory), process_resource());
        };
        definition.create_instance_ = [](void* stored_factory,
                                          std::pmr::memory_resource* resource) -> void* {
            return construct_pmr_object<t_type>(resource, (*static_cast<stored_type*>(stored_factory))());
        };
        definition.destroy_instance_ = [](void* instance,
                                           std::pmr::memory_resource* resource) noexcept {
            destroy_pmr_object(static_cast<t_type*>(instance), resource);
        };
        return definition;
    }

    [[nodiscard]] const void* type_key() const noexcept {
        return type_key_;
    }

    [[nodiscard]] bool valid() const noexcept {
        return type_key_ != nullptr && factory_ != nullptr && destroy_factory_ != nullptr &&
               create_instance_ != nullptr && destroy_instance_ != nullptr;
    }

private:
    friend class worker_state_registry;

    using destroy_factory_type = void (*)(void*) noexcept;
    using create_instance_type = void* (*)(void*, std::pmr::memory_resource*);
    using destroy_instance_type = void (*)(void*, std::pmr::memory_resource*) noexcept;

    worker_state_definition() noexcept = default;

    const void* type_key_{nullptr};
    void* factory_{nullptr};
    destroy_factory_type destroy_factory_{nullptr};
    create_instance_type create_instance_{nullptr};
    destroy_instance_type destroy_instance_{nullptr};
};

template <typename definitions_type>
void append_worker_state_definition(definitions_type& definitions, worker_state_definition&& definition) {
    if (!definition.valid()) {
        throw std::invalid_argument("worker state definition is invalid");
    }
    for (const auto& existing : definitions) {
        if (existing.type_key() == definition.type_key()) {
            throw std::invalid_argument("worker state type is already registered");
        }
    }
    definitions.push_back(std::move(definition));
}

template <typename definitions_type>
void validate_worker_state_definitions(const definitions_type& definitions) {
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        if (!definitions[index].valid()) {
            throw std::invalid_argument("worker state definition is invalid");
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (definitions[previous].type_key() == definitions[index].type_key()) {
                throw std::invalid_argument("worker state type is already registered");
            }
        }
    }
}

// The per-worker instances. web_worker_runtime explicitly initializes and
// destroys the registry inside its active worker identity window; a throwing
// factory fails startup before the worker dispatches callbacks or requests.
// Registration order remains the lifetime order (destruction is reversed),
// while a separate immutable type index gives request-time lookup one
// allocation-free binary search.
class worker_state_registry final {
public:
    worker_state_registry(
        std::pmr::memory_resource* resource, std::span<const worker_state_definition> definitions)
        : resource_(validated_resource(resource, definitions)),
          definitions_(definitions),
          entries_(resource_),
          type_index_(resource_) {}

    worker_state_registry(const worker_state_registry&) = delete;
    worker_state_registry& operator=(const worker_state_registry&) = delete;
    worker_state_registry(worker_state_registry&&) = delete;
    worker_state_registry& operator=(worker_state_registry&&) = delete;

    ~worker_state_registry() {
        if (initialized_) {
            std::terminate();
        }
    }

    void initialize() {
        if (initialized_) {
            throw std::logic_error("worker state registry is already initialized");
        }
        validate_worker_state_definitions(definitions_);
        entries_.reserve(definitions_.size());
        type_index_.reserve(definitions_.size());
        try {
            for (const auto& definition : definitions_) {
                entries_.push_back(instance_entry_type{definition.type_key_,
                    definition.create_instance_(definition.factory_, resource_),
                    definition.destroy_instance_});
                type_index_.push_back(type_index_entry_type{definition.type_key_, entries_.size() - 1});
            }
            std::ranges::sort(type_index_, std::less<const void*>{}, &type_index_entry_type::type_key_);
        } catch (...) {
            destroy_entries();
            throw;
        }
        initialized_ = true;
    }

    void shutdown() noexcept {
        if (!initialized_) {
            return;
        }
        destroy_entries();
        initialized_ = false;
    }

    [[nodiscard]] void* instance(const void* type_key) const noexcept {
        const auto match = std::ranges::lower_bound(
            type_index_, type_key, std::less<const void*>{}, &type_index_entry_type::type_key_);
        if (match == type_index_.end() || match->type_key_ != type_key) {
            return nullptr;
        }
        return entries_[match->entry_index_].instance_;
    }

private:
    // Called from the first member initializer so malformed startup input cannot
    // allocate container bookkeeping on standard libraries with debug proxies.
    [[nodiscard]] static std::pmr::memory_resource* validated_resource(
        std::pmr::memory_resource* resource, std::span<const worker_state_definition> definitions) {
        validate_worker_state_definitions(definitions);
        return pmr_resource_or_default(resource);
    }

    struct instance_entry_type final {
        const void* type_key_{nullptr};
        void* instance_{nullptr};
        worker_state_definition::destroy_instance_type destroy_{nullptr};
    };

    struct type_index_entry_type final {
        const void* type_key_{nullptr};
        std::size_t entry_index_{0};
    };

    void destroy_entries() noexcept {
        type_index_.clear();
        for (std::size_t i = entries_.size(); i > 0; --i) {
            auto& entry_value = entries_[i - 1];
            if (entry_value.instance_ != nullptr && entry_value.destroy_ != nullptr) {
                entry_value.destroy_(entry_value.instance_, resource_);
            }
        }
        entries_.clear();
    }

    std::pmr::memory_resource* resource_;
    std::span<const worker_state_definition> definitions_;
    std::pmr::vector<instance_entry_type> entries_;
    std::pmr::vector<type_index_entry_type> type_index_;
    bool initialized_{false};
};

}  // namespace ruvia::detail
