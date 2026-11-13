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
#include <memory>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/web/detail/http/context/request_bindings.h"

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
        auto* const factory = std::exchange(factory_, nullptr);
        auto* const destroy = std::exchange(destroy_factory_, nullptr);
        if (factory != nullptr && destroy != nullptr) {
            destroy(factory);
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
        definition.type_key_ = detail::type_key<t_type>();
        definition.factory_ = new stored_type(std::forward<factory_type>(factory));
        // Named apart from the enclosing `factory` parameter: these lambdas are
        // captureless and receive the erased pointer, not that object.
        definition.destroy_factory_ = [](void* stored_factory) noexcept {
            delete static_cast<stored_type*>(stored_factory);
        };
        definition.create_instance_ = [](void* stored_factory) -> void* {
            return new t_type((*static_cast<stored_type*>(stored_factory))());
        };
        definition.destroy_instance_ = [](void* instance) noexcept {
            delete static_cast<t_type*>(instance);
        };
        return definition;
    }

    // A caller-owned instance lent to one worker's registry: initialize()
    // adopts it and shutdown() leaves it to its owner.
    template <typename t_type>
    [[nodiscard]] static worker_state_definition borrow(t_type& instance) noexcept {
        static_assert(!std::is_const_v<t_type>, "a borrowed worker state must be mutable");
        worker_state_definition definition;
        definition.type_key_ = detail::type_key<t_type>();
        definition.factory_ = std::addressof(instance);
        definition.destroy_factory_ = [](void*) noexcept {};
        definition.create_instance_ = [](void* borrowed) -> void* { return borrowed; };
        definition.destroy_instance_ = [](void*) noexcept {};
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
    using create_instance_type = void* (*)(void*);
    using destroy_instance_type = void (*)(void*) noexcept;

    worker_state_definition() noexcept = default;

    const void* type_key_{nullptr};
    void* factory_{nullptr};
    destroy_factory_type destroy_factory_{nullptr};
    create_instance_type create_instance_{nullptr};
    destroy_instance_type destroy_instance_{nullptr};
};

// The single startup validation point for worker state recipes: the registry
// only ever receives definitions that passed through here.
inline void append_worker_state_definition(
    std::vector<worker_state_definition>& definitions, worker_state_definition&& definition) {
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

// The per-worker instances. worker_capabilities::start()/stop() initialize and
// destroy the registry on its worker; a throwing factory fails startup before
// the worker dispatches callbacks or requests.
// Registration order remains the lifetime order (destruction is reversed),
// while a separate immutable type index gives request-time lookup one
// allocation-free binary search.
class worker_state_registry final {
public:
    // `definitions` were validated by append_worker_state_definition.
    explicit worker_state_registry(std::span<const worker_state_definition> definitions) noexcept
        : definitions_(definitions) {}

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
        entries_.reserve(definitions_.size());
        type_index_.reserve(definitions_.size());
        try {
            for (const auto& definition : definitions_) {
                auto* const instance_value = definition.create_instance_(definition.factory_);
                entries_.push_back(instance_entry_type{instance_value, definition.destroy_instance_});
                type_index_.push_back(type_index_entry_type{definition.type_key_, instance_value});
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
        return match->instance_;
    }

private:
    struct instance_entry_type final {
        void* instance_{nullptr};
        worker_state_definition::destroy_instance_type destroy_{nullptr};
    };

    struct type_index_entry_type final {
        const void* type_key_{nullptr};
        void* instance_{nullptr};
    };

    void destroy_entries() noexcept {
        type_index_.clear();
        for (std::size_t i = entries_.size(); i > 0; --i) {
            auto& entry_value = entries_[i - 1];
            auto* const instance = std::exchange(entry_value.instance_, nullptr);
            auto* const destroy = std::exchange(entry_value.destroy_, nullptr);
            if (instance != nullptr && destroy != nullptr) {
                destroy(instance);
            }
        }
        entries_.clear();
    }

    std::span<const worker_state_definition> definitions_;
    std::vector<instance_entry_type> entries_;
    std::vector<type_index_entry_type> type_index_;
    bool initialized_{false};
};

}  // namespace ruvia::detail
