#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/http/borrowed_text.h"
// Worker-local user state: application::use_worker_state<T>() registers one recipe, each
// worker builds its own instance from it, and context::worker_state<T>() /
// web_worker_context::worker_state<T>() hand the instance back on that worker.
// This generalizes the db_registry pattern (one connection-pool-like object per
// single-threaded worker) to application-owned types: the instance is only
// ever touched from its worker's thread, so it needs no synchronization.

#include <algorithm>
#include <exception>
#include <functional>
#include <span>
#include <stdexcept>
#include <vector>

#include "ruvia/core/operation_options.h"

namespace ruvia::detail {

template <typename t_type>
struct type_key_tag final {
    inline static constexpr std::byte value{};
};

// One stable address per type, used as the binding's identity. Cheaper and more
// robust than typeid: no RTTI requirement and no cross-boundary name equality.
template <typename t_type>
[[nodiscard]] const void* type_key() noexcept {
    return &type_key_tag<std::remove_cvref_t<t_type>>::value;
}

// Two kinds share one intrusive list but never answer each other's lookups.
// c.req().validated<T>() means "a validator produced and checked this"; if a
// hand-bound request-state value of the same type could satisfy it, that
// guarantee would silently become a lie. Keeping the kinds disjoint preserves
// both contracts on one mechanism.
enum class request_binding_kind : std::uint8_t {
    validated_model,
    request_state,
};

class request_bindings;

struct request_binding_node final {
    const void* type_key_;
    const void* value_;
    request_binding_node* previous_;
    request_binding_kind kind_;
};

// The RAII guard a binder holds for as long as the value must stay visible.
// Neither copyable nor movable: the node it owns is linked into an intrusive
// stack by address, so it must not be relocated, and its scope IS the binding's
// lifetime. Held in the binding coroutine's frame across co_await next().
template <typename t_type>
class request_binding_guard final {
public:
    request_binding_guard(const request_binding_guard&) = delete;
    request_binding_guard& operator=(const request_binding_guard&) = delete;
    request_binding_guard(request_binding_guard&&) = delete;
    request_binding_guard& operator=(request_binding_guard&&) = delete;
    ~request_binding_guard() noexcept;

private:
    friend class request_bindings;

    request_binding_guard(request_bindings& bindings, const t_type& value, request_binding_kind kind) noexcept;

    request_bindings* bindings_;
    request_binding_node node_;
};

// context owns only the intrusive head. Every binder owns its value and its
// binding node, so nested next() calls form a naturally scoped, allocation-free
// stack that unwinds in strict LIFO order on success or failure. A binding is a
// capability visible only to its binder's downstream dynamic next() scope;
// upstream middleware cannot retain or observe it later.
class request_bindings final {
public:
    request_bindings() noexcept = default;
    ~request_bindings() noexcept {
        if (head_ != nullptr) {
            std::terminate();
        }
    }
    request_bindings(const request_bindings&) = delete;
    request_bindings& operator=(const request_bindings&) = delete;

    template <typename t_type>
    [[nodiscard]] const std::remove_cvref_t<t_type>* try_find(request_binding_kind kind) const noexcept {
        const auto* node_value = find_node<t_type>(kind);
        return node_value == nullptr ? nullptr
                                     : static_cast<const std::remove_cvref_t<t_type>*>(node_value->value_);
    }

    template <typename t_type>
    [[nodiscard]] const std::remove_cvref_t<t_type>& get_validated() const {
        return find_required<t_type>(
            request_binding_kind::validated_model, "validated request model is not available");
    }
    template <typename t_type>
    [[nodiscard]] const std::remove_cvref_t<t_type>* try_get_state() const noexcept {
        return try_find<t_type>(request_binding_kind::request_state);
    }

    template <typename t_type>
    [[nodiscard]] const std::remove_cvref_t<t_type>& get_state() const {
        return find_required<t_type>(request_binding_kind::request_state, "request state is not bound for this type");
    }

    template <typename t_type>
    [[nodiscard]] request_binding_guard<t_type> bind_validated(const t_type& value) {
        return request_binding_guard<t_type>(*this, value, request_binding_kind::validated_model);
    }
    template <typename t_type>
        requires(!std::is_lvalue_reference_v<t_type>)
    request_binding_guard<std::remove_cvref_t<t_type>> bind_validated(t_type&&) = delete;

    template <typename t_type>
    [[nodiscard]] request_binding_guard<t_type> bind_state(const t_type& value) {
        return request_binding_guard<t_type>(*this, value, request_binding_kind::request_state);
    }
    template <typename t_type>
        requires(!std::is_lvalue_reference_v<t_type>)
    request_binding_guard<std::remove_cvref_t<t_type>> bind_state(t_type&&) = delete;

private:
    template <typename t_type>
    friend class request_binding_guard;
    void push(request_binding_node& node_value) noexcept {
        node_value.previous_ = head_;
        head_ = &node_value;
    }

    void pop(request_binding_node& node_value) noexcept {
        if (head_ != &node_value) {
            std::terminate();
        }
        head_ = node_value.previous_;
    }

    template <typename t_type>
    [[nodiscard]] const request_binding_node* find_node(request_binding_kind kind) const noexcept {
        const auto* key = type_key<std::remove_cvref_t<t_type>>();
        for (auto* node_value = head_; node_value != nullptr; node_value = node_value->previous_) {
            if (node_value->type_key_ == key && node_value->kind_ == kind) {
                return node_value;
            }
        }
        return nullptr;
    }

    template <typename t_type>
    [[nodiscard]] const std::remove_cvref_t<t_type>& find_required(
        request_binding_kind kind, const char* missing_message) const {
        const auto* found = try_find<t_type>(kind);
        if (found == nullptr) {
            throw std::logic_error(missing_message);
        }
        return *found;
    }

    request_binding_node* head_{nullptr};
};

template <typename t_type>
request_binding_guard<t_type>::request_binding_guard(
    request_bindings& bindings, const t_type& value, request_binding_kind kind) noexcept
    : bindings_(&bindings),
      node_{type_key<t_type>(), &value, nullptr, kind} {
    bindings_->push(node_);
}

template <typename t_type>
request_binding_guard<t_type>::~request_binding_guard() noexcept {
    bindings_->pop(node_);
}
}  // namespace ruvia::detail

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

namespace ruvia::detail {

class worker_state_registry;

// A request and a posted job borrow the same worker-owned capabilities. No
// dispatcher or cancellation ownership is copied until an explicit offload.
class worker_context_capabilities final {
public:
    worker_context_capabilities(const worker_ref& worker_value, const ruvia::stop_token& stop_token_value,
        const worker_state_registry* worker_states) noexcept
        : worker_(worker_value),
          stop_token_(stop_token_value),
          worker_states_(worker_states) {}
    worker_context_capabilities(worker_ref&&, const ruvia::stop_token&, const worker_state_registry*) = delete;
    worker_context_capabilities(const worker_ref&, ruvia::stop_token&&, const worker_state_registry*) = delete;
    worker_context_capabilities(worker_ref&&, ruvia::stop_token&&, const worker_state_registry*) = delete;

    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }

    [[nodiscard]] const ruvia::stop_token& stop_token() const noexcept {
        return stop_token_;
    }

    [[nodiscard]] const worker_state_registry* worker_states() const noexcept {
        return worker_states_;
    }

    // A router serving the request substitutes its own registry for the
    // duration of its dispatch.
    void bind_worker_states(const worker_state_registry* worker_states) noexcept {
        worker_states_ = worker_states;
    }

    template <typename state_type>
    [[nodiscard]] state_type& worker_state() const {
        return *static_cast<state_type*>(worker_state_instance(type_key<state_type>()));
    }

    // Runs on the blocking pool of this worker's event loop; without one the
    // result reports blocking_status::unavailable. A supplied stop token is
    // combined with this worker's cancellation token for the wait.
    template <typename callable_type>
    [[nodiscard]] task<blocking_result<std::invoke_result_t<callable_type&>>> run_blocking(
        callable_type callable, operation_options options) const {
        if (!options.stop_token_.has_state()) {
            options.stop_token_ = stop_token_;
            return ruvia::run_blocking(worker_, std::move(options), std::move(callable));
        }
        return run_blocking_combined(worker_, stop_token_, std::move(options), std::move(callable));
    }

private:
    template <typename callable_type>
    [[nodiscard]] static task<blocking_result<std::invoke_result_t<callable_type&>>> run_blocking_combined(
        worker_ref worker_value, ruvia::stop_token worker_token, operation_options options, callable_type callable) {
        const stop_group stop_owner(worker_token, options.stop_token_);
        options.stop_token_ = stop_owner.token();
        co_return co_await ruvia::run_blocking(std::move(worker_value), std::move(options), std::move(callable));
    }

    [[nodiscard]] void* worker_state_instance(const void* type_key) const;

    const worker_ref& worker_;
    const ruvia::stop_token& stop_token_;
    const worker_state_registry* worker_states_;
};

}  // namespace ruvia::detail

namespace ruvia {

class http_client_ref;
#ifdef RUVIA_ENABLE_DATABASE
class db_session;
#endif
#ifdef RUVIA_ENABLE_REDIS
class redis_session;
#endif

namespace detail {
class web_worker_dispatch;
class worker_client_registry_view;
class worker_state_registry;
}  // namespace detail
class web_worker_context final {
public:
    web_worker_context(const web_worker_context&) = delete;
    web_worker_context& operator=(const web_worker_context&) = delete;
    web_worker_context(web_worker_context&&) = delete;
    web_worker_context& operator=(web_worker_context&&) = delete;

    [[nodiscard]] const worker_ref& worker() const& noexcept RUVIA_LIFETIMEBOUND;
    const worker_ref& worker() const&& = delete;
    [[nodiscard]] ::ruvia::stop_token stop_token() const noexcept;

    template <typename state_type>
    [[nodiscard]] state_type& worker_state() const {
        return capabilities_.worker_state<state_type>();
    }

    // The callable owns its inputs and must not borrow worker state. Completion
    // resumes here; worker shutdown and options.timeout_ end the wait, not the callable.
    // Rejection and callable exceptions are retained in blocking_result; inspect
    // status() or use std::move(result).take_value() to throw. The worker stop token
    // is bound automatically and combined with options.stop_token_. With application::blocking_pool(nullptr) the
    // result reports blocking_status::unavailable.
    template <typename callable_type>
    [[nodiscard]] task<blocking_result<std::invoke_result_t<callable_type&>>> run_blocking(
        callable_type callable, operation_options options = {}) const {
        return capabilities_.run_blocking(std::move(callable), std::move(options));
    }

#ifdef RUVIA_ENABLE_DATABASE
    [[nodiscard]] db_session db() const;
    [[nodiscard]] db_session db(std::string_view alias) const;
#endif
#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] redis_session redis() const;
    [[nodiscard]] redis_session redis(std::string_view alias) const;
#endif
    [[nodiscard]] http_client_ref client() const;
    [[nodiscard]] http_client_ref client(std::string_view alias) const;

private:
    friend class detail::web_worker_dispatch;

    web_worker_context(const worker_ref& worker_value,
        const detail::worker_client_registry_view& client_registries,
        const detail::worker_state_registry* worker_states, const ::ruvia::stop_token& stop_token_value) noexcept;
    web_worker_context(worker_ref&&, const detail::worker_client_registry_view&,
        const detail::worker_state_registry*, const ::ruvia::stop_token&) = delete;
    web_worker_context(const worker_ref&, const detail::worker_client_registry_view&,
        const detail::worker_state_registry*, ::ruvia::stop_token&&) = delete;
    web_worker_context(worker_ref&&, const detail::worker_client_registry_view&,
        const detail::worker_state_registry*, ::ruvia::stop_token&&) = delete;

    // The server worker owns these values and joins every posted job before
    // retiring them. Contexts borrow them so starting a job does not copy
    // endpoint or cancellation-state ownership on the worker thread.
    detail::worker_context_capabilities capabilities_;
    const detail::worker_client_registry_view& client_registries_;
    // Each posted callback gets an independent operation lifetime. Declared
    // last so cold frames are destroyed before the callback context disappears.
    mutable ::ruvia::operation_scope operation_scope_;
};

using web_worker_post_result = post_outcome<task<void>(web_worker_context&)>;

struct web_worker_stats final {
    std::uint64_t accepted_{0};
    std::uint64_t queue_full_{0};
    std::uint64_t worker_stopping_{0};
    std::uint64_t completed_{0};
    std::uint64_t failed_{0};
    std::size_t outstanding_{0};
};

class web_worker_ref final {
public:
    web_worker_ref() noexcept = default;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] bool is_current() const noexcept;
    [[nodiscard]] worker_id_type id() const noexcept;
    [[nodiscard]] web_worker_stats stats() const noexcept;

    template <typename fn_type>
        requires std::invocable<std::decay_t<fn_type>&, web_worker_context&> &&
                 std::same_as<std::invoke_result_t<std::decay_t<fn_type>&, web_worker_context&>,
                     task<void>>
    [[nodiscard]] web_worker_post_result post(fn_type&& fn) const {
        return post_task(move_only_function<task<void>(web_worker_context&)>(std::forward<fn_type>(fn)));
    }

private:
    friend class detail::web_worker_dispatch;

    web_worker_ref(std::shared_ptr<detail::web_worker_dispatch> dispatch) noexcept;

    [[nodiscard]] web_worker_post_result post_task(
        move_only_function<task<void>(web_worker_context&)> task) const;

    // The handle owns a stable terminal endpoint. Server shutdown closes it;
    // retaining a handle cannot retain the server or its io_context.
    std::shared_ptr<detail::web_worker_dispatch> dispatch_;
};

}  // namespace ruvia
