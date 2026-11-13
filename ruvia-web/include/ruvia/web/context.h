#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/model.h"

// Worker-local user state: application::use_worker_state<T>() registers one recipe, each
// worker builds its own instance from it, and context::worker_state<T>() /
// web_worker_context::worker_state<T>() hand the instance back on that worker.
// This generalizes the db_registry pattern (one connection-pool-like object per
// single-threaded worker) to application-owned types: the instance is only
// ever touched from its worker's thread, so it needs no synchronization.

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

class context;

// Declared before body admission, not executed as middleware. Route and
// controller ceilings can only tighten the server limit; the strictest wins.
// Global use()/use_at() reject this route-only policy.
template <std::size_t max_bytes>
class body_limit final {
public:
    static_assert(max_bytes > 0, "body limit must be greater than 0");
    static constexpr std::size_t ruvia_request_body_limit_bytes = max_bytes;
};

struct middleware_scope_options final {
    borrowed_text prefix_{};
};

namespace detail {
struct next_access;
struct middleware_chain;

struct next_state final {
    enum class invocation_type : std::uint8_t {
        ready,
        repeated,
        expired,
    };

    struct control_type final {
        enum class phase_type : std::uint8_t {
            fresh,
            invoked,
            expired,
        };

        [[nodiscard]] invocation_type begin_invocation() noexcept {
            if (phase_ == phase_type::fresh) {
                phase_ = phase_type::invoked;
                return invocation_type::ready;
            }
            return phase_ == phase_type::invoked ? invocation_type::repeated : invocation_type::expired;
        }

        void expire() noexcept {
            phase_ = phase_type::expired;
        }

    private:
        phase_type phase_{phase_type::fresh};
    };

    // The chain being walked, the request it runs for, this hop's single-shot
    // control block, and the frame index the continuation resumes at.
    const middleware_chain* chain_{nullptr};
    context* context_{nullptr};
    control_type* control_{nullptr};
    std::size_t index_{0};
    invocation_type invocation_{invocation_type::ready};
};

using next_invoke_type = task<void> (*)(next_state);
}  // namespace detail

class next final {
public:
    class awaitable final {
        class awaiter final {
        public:
            awaiter(const awaiter&) = delete;
            awaiter& operator=(const awaiter&) = delete;
            awaiter(awaiter&&) = delete;
            awaiter& operator=(awaiter&&) = delete;

            [[nodiscard]] bool await_ready() const noexcept {
                return awaiter_.await_ready();
            }

            [[nodiscard]] std::coroutine_handle<> await_suspend(
                std::coroutine_handle<> continuation) noexcept {
                return awaiter_.await_suspend(continuation);
            }

            void await_resume() {
                awaiter_.await_resume();
            }

            const awaiter* operator&() const = delete;
            awaiter* operator&() = delete;

        private:
            friend class awaitable;

            explicit awaiter(task<void>&& task_value)
                : awaiter_(std::move(task_value).operator co_await()) {}

            decltype(std::declval<task<void>>().operator co_await()) awaiter_;
        };

    public:
        awaitable(const awaitable&) = delete;
        awaitable& operator=(const awaitable&) = delete;
        awaitable(awaitable&&) = delete;
        awaitable& operator=(awaitable&&) = delete;

        [[nodiscard]] awaiter operator co_await() && {
            auto state_value = state_;
            if (phase_ == phase_type::awaited) {
                state_value.invocation_ = detail::next_state::invocation_type::repeated;
            }
            phase_ = phase_type::awaited;
            return awaiter(invoke_(state_value));
        }
        [[nodiscard]] auto operator co_await() & = delete;
        [[nodiscard]] auto operator co_await() const& = delete;
        [[nodiscard]] auto operator co_await() const&& = delete;
        const awaitable* operator&() const = delete;
        awaitable* operator&() = delete;

    private:
        friend class next;

        constexpr awaitable(detail::next_state state_value, detail::next_invoke_type invoke) noexcept
            : state_(state_value),
              invoke_(invoke) {}

        detail::next_state state_;
        detail::next_invoke_type invoke_{nullptr};
        enum class phase_type : std::uint8_t {
            fresh,
            awaited,
        };
        phase_type phase_{phase_type::fresh};
    };

    next(const next&) = delete;
    next& operator=(const next&) = delete;
    next(next&&) = delete;
    next& operator=(next&&) = delete;

    [[nodiscard]] awaitable operator()() &;
    [[nodiscard]] awaitable operator()() const& = delete;
    [[nodiscard]] awaitable operator()() && = delete;
    [[nodiscard]] awaitable operator()() const&& = delete;
    const next* operator&() const = delete;
    next* operator&() = delete;

private:
    friend struct detail::next_access;

    constexpr next(detail::next_state state_value, detail::next_invoke_type invoke) noexcept
        : state_(state_value),
          invoke_(invoke) {}

    detail::next_state state_;
    detail::next_invoke_type invoke_{nullptr};
};

// A middleware is any final class with a public
//     task<void> invoke(context&, next&) [const]
// member. It derives from nothing: the framework composes it through per-type
// entry points and reads its route properties from static members. A
// middleware that produces the response calls context::respond() before it
// returns, so the chain has exactly one response path.
//
// application::use<T>() instantiates the descriptor at the call site. Built-in
// middleware headers already include that machinery. A custom middleware's
// use<T>() call site needs only context.h or app.h.
// This header stays free of context so app.h does not pull it in.
template <typename middleware_type>
concept middleware = std::is_class_v<middleware_type> && std::is_final_v<middleware_type> &&
                     (requires { static_cast<task<void> (middleware_type::*)(context&, next&)>(&middleware_type::invoke); } || requires { static_cast<task<void> (middleware_type::*)(context&, next&) const>(&middleware_type::invoke); });
template <typename policy_type>
concept route_policy =
    std::is_class_v<policy_type> && std::is_final_v<policy_type> && !middleware<policy_type> &&
    (requires {
        { policy_type::ruvia_request_body_limit_bytes } -> std::convertible_to<std::size_t>; } || requires {
            { policy_type::ruvia_stream_request_body } -> std::convertible_to<bool>; });

template <typename modifier_type>
concept route_modifier = middleware<modifier_type> || route_policy<modifier_type>;

// Internal startup-time middleware descriptor.

namespace detail {

class controller_middleware_descriptor;

template <middleware middleware_type, typename args_type>
[[nodiscard]] controller_middleware_descriptor describe_middleware(const void* args) noexcept;

template <route_policy policy_type>
[[nodiscard]] constexpr controller_middleware_descriptor describe_route_policy() noexcept;

class controller_middleware_descriptor final {
public:
    using invoke_type = route_middleware::invoke_type;
    // `args` points at the argument tuple captured by use<T>(args...), or is
    // null when the middleware is default constructed. The tuple belongs to the
    // owned_middleware_descriptor the app configuration keeps, which outlives
    // every plan compiled from it and every instance a worker's route table
    // materializes from this descriptor.
    using create_type = void* (*)(const void* args);
    using destroy_type = void (*)(void*) noexcept;

    [[nodiscard]] invoke_type invoke() const noexcept {
        return invoke_;
    }

    [[nodiscard]] create_type create() const noexcept {
        return create_;
    }

    [[nodiscard]] const void* args() const noexcept {
        return args_;
    }

    [[nodiscard]] destroy_type destroy() const noexcept {
        return destroy_;
    }

    [[nodiscard]] const void* validated_model_type_key() const noexcept {
        return validated_model_type_key_;
    }

    // Empty means app-wide. Otherwise the middleware runs exactly on requests
    // whose path is under this prefix. The compiled route plan decides that per
    // route and only checks the request path at dispatch for a route serving
    // paths on both sides of the scope. The text is registration-owned and
    // outlives the plan.
    [[nodiscard]] std::string_view prefix() const noexcept {
        return prefix_;
    }

    [[nodiscard]] controller_middleware_descriptor scoped_to(std::string_view prefix) const noexcept {
        auto scoped = *this;
        scoped.prefix_ = prefix;
        return scoped;
    }

    // See middleware_runs_on_unmatched_requests(): true means this middleware also
    // wraps the 404/405/501 terminal, not just matched routes.
    [[nodiscard]] bool runs_on_unmatched_requests() const noexcept {
        return runs_on_unmatched_requests_;
    }

    // 0 when this middleware declares no ceiling.
    [[nodiscard]] std::size_t request_body_limit() const noexcept {
        return request_body_limit_;
    }

    [[nodiscard]] bool uses_route_rate_limit() const noexcept {
        return uses_route_rate_limit_;
    }

    [[nodiscard]] bool stream_request_body() const noexcept {
        return stream_request_body_;
    }

private:
    template <middleware middleware_type, typename args_type>
    friend controller_middleware_descriptor describe_middleware(const void* args) noexcept;
    template <route_policy policy_type>
    friend constexpr controller_middleware_descriptor describe_route_policy() noexcept;

    constexpr controller_middleware_descriptor(invoke_type invoke, create_type create, destroy_type destroy,
        const void* args, const void* validated_model_type_key, bool uses_route_rate_limit,
        bool runs_on_unmatched_requests, std::size_t request_body_limit,
        bool stream_request_body = false) noexcept
        : invoke_(invoke),
          create_(create),
          destroy_(destroy),
          args_(args),
          validated_model_type_key_(validated_model_type_key),
          uses_route_rate_limit_(uses_route_rate_limit),
          request_body_limit_(request_body_limit),
          runs_on_unmatched_requests_(runs_on_unmatched_requests),
          stream_request_body_(stream_request_body) {}
    invoke_type invoke_{nullptr};
    create_type create_{nullptr};
    destroy_type destroy_{nullptr};
    const void* args_{nullptr};
    const void* validated_model_type_key_{nullptr};
    std::string_view prefix_{};
    bool uses_route_rate_limit_{false};
    std::size_t request_body_limit_{0};
    bool runs_on_unmatched_requests_{false};
    bool stream_request_body_{false};
};

// An app-level use<T>(args...) registration: the descriptor plus ownership of
// the argument tuple it points at. The app configuration holds it for its own
// lifetime, which spans every route plan compiled from it; descriptors copied
// into plans and route tables only borrow the tuple. A registration without
// arguments owns nothing.
class owned_middleware_descriptor final {
public:
    using release_args_type = void (*)(const void* args) noexcept;

    owned_middleware_descriptor(controller_middleware_descriptor descriptor, release_args_type release_args) noexcept
        : descriptor_(descriptor),
          release_args_(release_args) {}

    owned_middleware_descriptor(const owned_middleware_descriptor&) = delete;
    owned_middleware_descriptor& operator=(const owned_middleware_descriptor&) = delete;

    owned_middleware_descriptor(owned_middleware_descriptor&& other) noexcept
        : prefix_(std::move(other.prefix_)),
          descriptor_(other.descriptor_.scoped_to(prefix_)),
          release_args_(std::exchange(other.release_args_, nullptr)) {}
    owned_middleware_descriptor& operator=(owned_middleware_descriptor&&) = delete;

    ~owned_middleware_descriptor() {
        if (auto* const release = std::exchange(release_args_, nullptr)) {
            release(descriptor_.args());
        }
    }

    [[nodiscard]] const controller_middleware_descriptor& descriptor() const noexcept {
        return descriptor_;
    }

    [[nodiscard]] owned_middleware_descriptor scoped_to(std::string_view prefix) && {
        prefix_.assign(prefix);
        descriptor_ = descriptor_.scoped_to(prefix_);
        return std::move(*this);
    }

private:
    std::string prefix_{};
    controller_middleware_descriptor descriptor_;
    release_args_type release_args_{nullptr};
};

}  // namespace detail

}  // namespace ruvia

namespace ruvia::detail {

template <middleware middleware_type>
[[nodiscard]] task<void> invoke_middleware(void* target, context& context_value, next& next_value) {
    if constexpr (requires {
                      static_cast<task<void> (middleware_type::*)(context&, next&)>(&middleware_type::invoke);
                  }) {
        constexpr auto method =
            static_cast<task<void> (middleware_type::*)(context&, next&)>(&middleware_type::invoke);
        return (static_cast<middleware_type*>(target)->*method)(context_value, next_value);
    } else {
        constexpr auto method =
            static_cast<task<void> (middleware_type::*)(context&, next&) const>(&middleware_type::invoke);
        return (static_cast<const middleware_type*>(target)->*method)(context_value, next_value);
    }
}

// Constructs the middleware from the arguments use<T>(args...) captured. The
// tuple is owned by the app configuration's owned_middleware_descriptor, so it
// is still readable every time a worker's route table materializes an instance
// -- including across a stop()/run() cycle, which rebuilds instances from the
// same descriptors. Registering without arguments stores no tuple at all: the
// descriptor's args are null and the default constructor runs.
template <typename middleware_type, typename args_type>
[[nodiscard]] void* create_middleware(const void* args) {
    if constexpr (std::tuple_size_v<args_type> == 0) {
        return static_cast<void*>(new middleware_type());
    } else {
        return std::apply(
            [](const auto&... values) {
                return static_cast<void*>(new middleware_type(values...));
            },
            *static_cast<const args_type*>(args));
    }
}

template <typename middleware_type>
void destroy_middleware(void* target) noexcept {
    delete static_cast<middleware_type*>(target);
}

template <typename args_type>
void release_middleware_args(const void* args) noexcept {
    delete static_cast<const args_type*>(args);
}

template <typename middleware_type>
[[nodiscard]] const void* middleware_validated_model_type_key() noexcept {
    if constexpr (requires { typename middleware_type::ruvia_validation_body_type; }) {
        return type_key<typename middleware_type::ruvia_validation_body_type>();
    } else {
        return nullptr;
    }
}

template <typename middleware_type>
[[nodiscard]] constexpr bool middleware_uses_route_rate_limit() noexcept {
    if constexpr (requires { middleware_type::ruvia_uses_route_rate_limit; }) {
        return middleware_type::ruvia_uses_route_rate_limit;
    } else {
        return false;
    }
}

// Whether this middleware is meaningful on a request that matched no route.
// A property of the middleware, not of where it is registered: security headers
// and request ids belong on a 404 response just as much as on a 200, while a
// validator or an authorization check has nothing to act on. Declared as
//     static constexpr bool ruvia_runs_on_unmatched_requests = true;
template <typename middleware_type>
[[nodiscard]] constexpr bool middleware_runs_on_unmatched_requests() noexcept {
    if constexpr (requires { middleware_type::ruvia_runs_on_unmatched_requests; }) {
        return middleware_type::ruvia_runs_on_unmatched_requests;
    } else {
        return false;
    }
}

// A request-body ceiling this middleware declares for the routes it is on, or 0
// for none. Read by the server BEFORE the body is accepted, which is why it
// cannot simply be code inside invoke(): by the time a middleware runs, the
// bytes it would have rejected are already buffered.
template <typename middleware_type>
[[nodiscard]] constexpr std::size_t middleware_request_body_limit() noexcept {
    if constexpr (requires { middleware_type::ruvia_request_body_limit_bytes; }) {
        return middleware_type::ruvia_request_body_limit_bytes;
    } else {
        return 0;
    }
}

template <typename middleware_type>
[[nodiscard]] constexpr bool middleware_stream_request_body() noexcept {
    if constexpr (requires { middleware_type::ruvia_stream_request_body; }) {
        return middleware_type::ruvia_stream_request_body;
    } else {
        return false;
    }
}

// The one description of a middleware type: its entry points and declared
// route properties, bound to the argument tuple (null when there is none).
template <middleware middleware_type, typename args_type>
[[nodiscard]] controller_middleware_descriptor describe_middleware(const void* args) noexcept {
    return controller_middleware_descriptor(&invoke_middleware<middleware_type>,
        &create_middleware<middleware_type, args_type>, &destroy_middleware<middleware_type>, args,
        middleware_validated_model_type_key<middleware_type>(), middleware_uses_route_rate_limit<middleware_type>(),
        middleware_runs_on_unmatched_requests<middleware_type>(), middleware_request_body_limit<middleware_type>(),
        middleware_stream_request_body<middleware_type>());
}

// A default-constructed middleware, as controller and route middleware lists
// declare it. It captures nothing, so it allocates nothing and owns nothing:
// route registration reruns on every route plan compile.
template <middleware middleware_type>
[[nodiscard]] controller_middleware_descriptor make_middleware_descriptor() noexcept {
    static_assert(std::is_default_constructible_v<middleware_type>,
        "middleware registered without arguments must be default constructible");
    return describe_middleware<middleware_type, std::tuple<>>(nullptr);
}

template <route_policy policy_type>
[[nodiscard]] constexpr controller_middleware_descriptor describe_route_policy() noexcept {
    return controller_middleware_descriptor(nullptr, nullptr, nullptr, nullptr, nullptr, false, false,
        middleware_request_body_limit<policy_type>(), middleware_stream_request_body<policy_type>());
}

template <route_modifier modifier_type>
[[nodiscard]] controller_middleware_descriptor make_route_modifier_descriptor() noexcept {
    if constexpr (route_policy<modifier_type>) {
        return describe_route_policy<modifier_type>();
    } else {
        return make_middleware_descriptor<modifier_type>();
    }
}

// Registers one app-level middleware type together with the arguments every
// instance of it is constructed from. Arguments are decayed and copied once,
// at registration; a middleware is built per worker route table, so they must
// stay readable for the app configuration's lifetime rather than the caller's
// scope. The returned owner destroys them. Registering without arguments
// stores no tuple.
template <middleware middleware_type, typename... args_types>
[[nodiscard]] owned_middleware_descriptor make_owned_middleware_descriptor(args_types&&... args) {
    static_assert(std::is_constructible_v<middleware_type, const std::decay_t<args_types>&...>,
        "middleware is not constructible from the arguments passed to use<T>(); a middleware "
        "registered without arguments must be default constructible");

    if constexpr (sizeof...(args_types) == 0) {
        return owned_middleware_descriptor(make_middleware_descriptor<middleware_type>(), nullptr);
    } else {
        using args_tuple_type = std::tuple<std::decay_t<args_types>...>;
        const auto* stored = new args_tuple_type(std::forward<args_types>(args)...);
        return owned_middleware_descriptor(
            describe_middleware<middleware_type, args_tuple_type>(stored), &release_middleware_args<args_tuple_type>);
    }
}

}  // namespace ruvia::detail

namespace ruvia {

struct csrf_protection_config final {
    std::string cookie_name_{"XSRF-TOKEN"};
    // 必须为脚本主动提交 token 的自定义字段；构造时拒绝 CORS-safelisted、
    // 浏览器保留及已知自动生成/代理字段（名称不区分大小写）。
    // 部署方还须保证代理不注入所选字段，且仅向可信 origin 授予携带凭据的
    // CORS 权限。不能用 Accept、Origin、Host 等环境信息代替主动提交的 token。
    std::string header_name_{"X-XSRF-TOKEN"};
};

// Stateless CSRF protection using the double-submit-cookie pattern (no
// server-side session store needed, so it works across SO_REUSEPORT workers).
// A safe request (GET/HEAD/OPTIONS) without a token cookie is issued a fresh
// one (readable by JavaScript so a SPA can echo it). An unsafe request must
// repeat that cookie's value in the header; a missing or mismatched token is
// rejected with 403. Register on a controller, group, or route that should
// enforce browser XSRF checks. Cookie and header names default to "XSRF-TOKEN"
// and "X-XSRF-TOKEN" and can be rebranded per app.
// This relies on the browser's custom-request-header boundary. It does not
// authenticate a user or protect an origin already trusted by credentialed CORS.
class csrf_protection final {
public:
    csrf_protection();
    explicit csrf_protection(const csrf_protection_config& config);

    csrf_protection(const csrf_protection&) = delete;
    csrf_protection& operator=(const csrf_protection&) = delete;
    csrf_protection(csrf_protection&&) = delete;
    csrf_protection& operator=(csrf_protection&&) = delete;

    task<void> invoke(context& c, next& next);

private:
    [[nodiscard]] static const csrf_protection_config& validate(const csrf_protection_config& source);

    std::string cookie_name_;
    std::string header_name_;
};

struct security_header final {
    std::string name_{};
    std::string value_{};
};
// Legacy browser XSS filters can introduce vulnerabilities in otherwise safe
// pages. Modern policy either emits X-XSS-Protection: 0 to disable the obsolete
// auditor, or omits the obsolete header entirely for clients that ignore it.
enum class xss_protection_header_policy : std::uint8_t {
    emit_disabled,
    omit,
};

enum class default_security_header_policy : std::uint8_t {
    omit,
    emit_default,
};

enum class security_header_conflict_policy : std::uint8_t {
    preserve_existing,
    replace_existing,
};

struct security_headers_config final {
    default_security_header_policy content_type_options_header_ =
        default_security_header_policy::emit_default;
    default_security_header_policy frame_options_header_ = default_security_header_policy::emit_default;
    // Emitted only for requests received over TLS. Plain HTTP responses must
    // never carry Strict-Transport-Security.
    default_security_header_policy strict_transport_security_header_ =
        default_security_header_policy::emit_default;
    xss_protection_header_policy xss_protection_header_ = xss_protection_header_policy::emit_disabled;

    std::string content_security_policy_{"default-src 'self'"};
    std::string referrer_policy_{"strict-origin-when-cross-origin"};
    std::string permissions_policy_{"geolocation=(), microphone=(), camera=()"};

    std::vector<security_header> custom_headers_{};
    security_header_conflict_policy existing_headers_ = security_header_conflict_policy::preserve_existing;
};

// Registered app-wide with the defaults as `app().use<security_headers_middleware>()`,
// or with an owning policy as `app().use<security_headers_middleware>(config)`.
class security_headers_middleware final {
public:
    // A 404 is a response to an attacker-reachable URL like any other, so it
    // needs the same CSP, frame and referrer policy a matched route gets. CORS
    // follows the same response-layer rule for unmatched requests.
    static constexpr bool ruvia_runs_on_unmatched_requests = true;

    security_headers_middleware();
    explicit security_headers_middleware(const security_headers_config& config);

    security_headers_middleware(const security_headers_middleware&) = delete;
    security_headers_middleware& operator=(const security_headers_middleware&) = delete;
    security_headers_middleware(security_headers_middleware&&) = delete;
    security_headers_middleware& operator=(security_headers_middleware&&) = delete;

    task<void> invoke(context& context, next& next);

private:
    bool emit_content_type_options_{true};
    bool emit_frame_options_{true};
    bool emit_strict_transport_security_{true};
    bool emit_disabled_xss_protection_{true};
    bool replace_existing_{false};
    std::string content_security_policy_;
    std::string referrer_policy_;
    std::string permissions_policy_;
    std::vector<http_header> custom_headers_;
};

// Buffer capacity required by format_rate_limit_address_key ("v6s/" + 32 hex
// digits + '/' + up to 10 scope digits).
inline constexpr std::size_t rate_limit_key_buffer_bytes = 48;
// Each worker preallocates this many sliding-window key entries when either
// the app-wide default rule or a route-specific rate-limit middleware is
// present. A key probes a bounded run of entries from its hash position; a
// key whose run holds only live entries of other keys takes the rule's
// overflow policy even when other parts of the table are free. An entry stays
// live until two of its key's windows have passed without a request.
inline constexpr std::size_t default_rate_limit_capacity_per_worker = 8192;

enum class rate_limit_overflow_policy : std::uint8_t {
    deny,
    allow,
};

// Per-worker, per-address sliding-window rule. Configuration remains a plain
// value; each owner validates it once before publishing runtime state.
// Each key's windows start at its first request and follow one another. A
// request is admitted while
//     previous * (unelapsed fraction of the current window) + current
// stays below max_requests_, where current and previous count the requests
// admitted in the current and the preceding window (a gap of two or more
// windows leaves previous at zero). The previous window's count fades out
// instead of vanishing at the boundary, so a client cannot spend one budget
// at the end of a window and a second right after it. Refused requests are
// not counted. A refusal's Retry-After is the earliest whole second at which
// the estimate admits again; X-RateLimit-Reset is when it has fallen to zero.
// Both app-wide and route rules key on the resolved client address: unscoped
// IPv6 shares a /64 budget, IPv4-mapped IPv6 shares its dotted IPv4 host budget,
// and scoped IPv6 keys on its full address plus numeric scope. Equivalent
// accepted IPv6 spellings do not create additional budgets.
struct rate_limit_rule final {
    std::size_t max_requests_{0};
    std::chrono::milliseconds window_{0};
    rate_limit_overflow_policy overflow_policy_{rate_limit_overflow_policy::deny};
};

struct rate_limit_config final {
    rate_limit_rule rule_{};
    std::size_t capacity_per_worker_{default_rate_limit_capacity_per_worker};
};

// Normalizes a client IP address into its per-client rate-limit key:
// - IPv4 and IPv4-mapped IPv6 share the canonical dotted IPv4 host key;
// - Unscoped IPv6 is grouped by its /64 network prefix ("v6/<16 hex>");
// - Scoped IPv6 keys on its full 128-bit address and numeric scope ("v6s/<32 hex>/<scope>").
[[nodiscard]] std::string_view format_rate_limit_address_key(
    const ip_address& address, std::span<char, rate_limit_key_buffer_bytes> buffer) noexcept;
[[nodiscard]] std::string_view format_rate_limit_address_key(
    std::string_view address, std::span<char, rate_limit_key_buffer_bytes> buffer) noexcept;

struct rate_limit_decision final {
    bool allowed_{true};
    std::size_t limit_{0};
    std::size_t remaining_{0};
    std::uint64_t retry_after_ms_{0};
    std::uint64_t reset_after_ms_{0};

    [[nodiscard]] constexpr bool allowed() const noexcept {
        return allowed_;
    }
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return allowed_;
    }
    [[nodiscard]] constexpr std::size_t limit() const noexcept {
        return limit_;
    }
    [[nodiscard]] constexpr std::size_t remaining() const noexcept {
        return remaining_;
    }
    [[nodiscard]] constexpr std::uint64_t retry_after_ms() const noexcept {
        return retry_after_ms_;
    }
    [[nodiscard]] constexpr std::uint64_t reset_after_ms() const noexcept {
        return reset_after_ms_;
    }
    [[nodiscard]] constexpr std::uint64_t retry_after_seconds() const noexcept {
        return retry_after_ms_ / 1000U + (retry_after_ms_ % 1000U == 0U ? 0U : 1U);
    }
    [[nodiscard]] constexpr std::uint64_t reset_after_seconds() const noexcept {
        return reset_after_ms_ / 1000U + (reset_after_ms_ % 1000U == 0U ? 0U : 1U);
    }

    friend constexpr bool operator==(const rate_limit_decision&, const rate_limit_decision&) noexcept = default;
};

void apply_rate_limit_headers(http_response& response, const rate_limit_decision& decision);

// Single-worker SipHash-1-3 sliding-window rate limiter table. Preallocates a
// power-of-two slot array at construction and performs zero allocations or
// locks on the decision path.
class sliding_window_rate_limiter final {
public:
    explicit sliding_window_rate_limiter(const rate_limit_config& config);
    explicit sliding_window_rate_limiter(
        std::size_t capacity_per_worker = default_rate_limit_capacity_per_worker);
    sliding_window_rate_limiter(const sliding_window_rate_limiter&) = delete;
    sliding_window_rate_limiter& operator=(const sliding_window_rate_limiter&) = delete;
    sliding_window_rate_limiter(sliding_window_rate_limiter&&) noexcept;
    sliding_window_rate_limiter& operator=(sliding_window_rate_limiter&&) noexcept;
    ~sliding_window_rate_limiter();

    [[nodiscard]] bool has_default_rule() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;

    [[nodiscard]] rate_limit_decision decide(std::string_view key,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept;
    [[nodiscard]] rate_limit_decision decide(const ip_address& address,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept;

    [[nodiscard]] rate_limit_decision decide(std::string_view key, const rate_limit_rule& rule,
        std::uintptr_t scope = 1,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept;
    [[nodiscard]] rate_limit_decision decide(const ip_address& address, const rate_limit_rule& rule,
        std::uintptr_t scope = 1,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept;

private:
    struct impl_type;
    std::unique_ptr<impl_type> impl_;
};
namespace detail {
// `declaration` identifies the rule's declaration: stacked rules on one route
// each count in their own window. Throws when the rule refuses the request.
void enforce_route_rate_limit(context& context_value, const void* declaration, const rate_limit_rule& rule);

}  // namespace detail

// A per-route rate limit, configured through the type:
//
//     RUVIA_GET("/ready", ready, ruvia::rate_limit<10, 1000>);
//
// Route and controller middleware lists name types, so a middleware registered
// there is default constructed and cannot take constructor arguments. Carrying
// the configuration as template parameters is how such a middleware is
// configured -- the values are constexpr, the type stays default constructible,
// and the chain is still finalized at startup with nothing allocated per
// request. A comma inside the template argument list is fine: the route macro
// pastes its arguments back together.
//
// The limit is scoped to the route it is registered on and keyed on the client
// address, so two routes carrying the same numbers count independently. Each
// declaration also counts on its own: an app or controller rule stacked over a
// route rule enforces both windows, never one shared counter. It does not
// replace application::rate_limit() -- both apply, so the stricter is what a
// caller meets, the same "narrower scope may only tighten" rule body_limit
// follows. Worker-local, like the app-wide rule, and counted with the same
// sliding-window estimate (see rate_limit_rule). A refused request is answered
// like one the app-wide rule refuses: 429 through the application error
// handler, with Retry-After and X-RateLimit-* fields.
template <std::size_t max_requests, std::int64_t window_ms,
    rate_limit_overflow_policy overflow_policy = rate_limit_overflow_policy::deny>
class rate_limit final {
public:
    static_assert(max_requests > 0, "route rate limit max requests must be greater than 0");
    static_assert(window_ms > 0, "route rate limit window must be greater than 0ms");

    static constexpr bool ruvia_uses_route_rate_limit = true;

    task<void> invoke(context& context_value, next& next_value) {
        detail::enforce_route_rate_limit(context_value, this, rule_);
        co_await next_value();
    }

private:
    static constexpr rate_limit_rule rule_{
        .max_requests_ = max_requests,
        .window_ = std::chrono::milliseconds(window_ms),
        .overflow_policy_ = overflow_policy,
    };
};

}  // namespace ruvia

namespace ruvia::detail {

class context_session_state;
struct session_access;

}  // namespace ruvia::detail

namespace ruvia {

// A request-local capability bound by session_middleware. The handle borrows
// context state and therefore cannot escape the request.
// Changes are persisted automatically before the response head (including a
// stream head or a websocket/CONNECT handshake) is sent. Once that commit
// begins, set(), clear(), and regenerate() throw std::logic_error; data()
// remains readable.
class session final {
public:
    [[nodiscard]] std::string_view data() const& noexcept;
    std::string_view data() const&& = delete;
    [[nodiscard]] bool empty() const noexcept {
        return data().empty();
    }
    // Every non-empty write replaces an existing session identity at commit.
    // Repeated writes in one request publish one new ID; reads keep the ID.
    void set(std::string_view data);
    void clear();
    // Rotate without changing the data, for an authentication or privilege
    // transition that does not write the session. set() already requests rotation.
    // Replacing an expired or revoked session fails with HTTP 409.
    void regenerate();

private:
    explicit session(detail::context_session_state& state_value) noexcept
        : state_(&state_value) {}

    detail::context_session_state* state_;
    friend class context;
};

}  // namespace ruvia

#ifdef RUVIA_ENABLE_REDIS

namespace ruvia {

// Server-side session backed by Redis (RUVIA_ENABLE_REDIS). Reads the configured
// cookie only when its ID is exactly 48 lowercase hexadecimal characters, the
// fixed length minted by this middleware; other IDs are treated as absent.
// Loads the blob at key_prefix_ + id (by default sess:<id>) into the context.
// Before the response head is sent, persists changes with the configured TTL
// or deletes the session. A new session mints a random ID in an HttpOnly cookie.
// The blob format is the application's; pair it with JSON if desired.
struct session_config final {
    std::string redis_alias_{"default"};
    std::string cookie_name_{"sid"};
    std::string key_prefix_{"sess:"};
    // Must be positive and representable in std::chrono::milliseconds;
    // otherwise construction throws std::invalid_argument.
    std::chrono::seconds ttl_{std::chrono::hours(24)};
};

class session_middleware final {
public:
    session_middleware();
    explicit session_middleware(const session_config& config);

    session_middleware(const session_middleware&) = delete;
    session_middleware& operator=(const session_middleware&) = delete;
    session_middleware(session_middleware&&) = delete;
    session_middleware& operator=(session_middleware&&) = delete;

    task<void> invoke(context& c, next& next);

private:
    friend struct detail::session_access;
    task<void> commit(context& c) const;

    [[nodiscard]] static const session_config& validate(const session_config& source);

    std::string redis_alias_;
    std::string cookie_name_;
    std::string key_prefix_;
    std::chrono::seconds ttl_;
};

}  // namespace ruvia

#endif  // RUVIA_ENABLE_REDIS

namespace ruvia {

// Route-level policy declaration that switches the matched route's request body
// from buffered to streamed (read via c.req().body_reader() or
// c.req().multipart_reader()). May be combined with either a buffered-response
// handler (task<http_response>) or a streaming-response handler (task<void>).
struct stream_body final {
    static constexpr bool ruvia_stream_request_body = true;
};
namespace detail {
struct streaming_access;
class operation_interrupt_source;
}  // namespace detail

class body_reader final {
private:
    friend struct detail::streaming_access;
    friend class multipart_reader;

    using read_type = detail::callback_ref<task<std::optional<std::span<const std::byte>>>(operation_options)>;

    body_reader(void* target, read_type::invoke_type read) noexcept
        : read_(detail::callback_access::make<task<std::optional<std::span<const std::byte>>>(operation_options)>(target, read)) {}

public:
    body_reader(const body_reader&) = delete;
    body_reader& operator=(const body_reader&) = delete;

    /// Reads the next chunk of the streamed request body.
    /// @warning The returned view borrows the connection read buffer and is only valid
    /// until the next read operation starts; copy it out if you need to retain it past then.
    /// An empty optional signals end-of-body. Only one read operation may run at a time;
    /// starting another before it completes throws because concurrent consumers cannot
    /// safely share the borrowed buffer. Unstarted operations may coexist.
    /// HTTP/1 enters the interim-write operation only while a 100 Continue is
    /// pending; later reads do not repeat that operation.
    /// `operation` 的超时或停止只结束本次等待，抛 system_error（timed_out /
    /// operation_canceled），不返回 EOF；已缓冲正文保留，可继续读取。
    /// 不取消响应方向，不放宽服务器已有正文超时；非正超时抛 invalid_argument。
    /// 启动的任务仍须等待完成后再释放宿主。
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read(operation_options operation = {}) &;
    task<std::optional<std::span<const std::byte>>> read(operation_options = {}) && = delete;

    /// Reads the next chunk as characters without charset conversion or UTF-8
    /// validation. Chunk boundaries may split encoded characters. Shares read()'s
    /// lifetime, linear operation and `operation` contract.
    [[nodiscard]] task<std::optional<std::string_view>> text(operation_options operation = {}) &;
    task<std::optional<std::string_view>> text(operation_options = {}) && = delete;

private:
    void bind_worker(const worker_ref& worker_value) noexcept {
        worker_ = &worker_value;
        operation_scope_.bind_worker(worker_value);
    }

    read_type read_;
    bool read_active_{false};
    const worker_ref* worker_{nullptr};
    ::ruvia::operation_scope operation_scope_;
};

// 提交成功后只有这两种正常结果：可生产正文，或整个响应已完成。
enum class stream_commit_result { body,
    completed };

class response_stream_writer final {
public:
    response_stream_writer(const response_stream_writer&) = delete;
    response_stream_writer& operator=(const response_stream_writer&) = delete;

    /// 显式提交响应头（包含 session 持久化）；成功返回 body 才可生产正文。
    /// HEAD、304 返回 completed，属于正常完成，不设置 aborted 或取消 token。
    /// commit/write/writeln/end 共用线性输出通道，并发启动抛 logic_error。
    /// 重复 commit 抛 logic_error；提交前失败仍可由框架替换为缓冲错误响应。
    /// 输出操作的 `operation` 超时或停止中止本请求流（HTTP/2、HTTP/3 重置流，
    /// HTTP/1 关闭连接；已写出部分无法撤回）并抛 system_error（timed_out /
    /// operation_canceled）；启动前已停止则不发出任何字节。非正超时抛
    /// invalid_argument；服务器写超时继续生效。
    [[nodiscard]] task<stream_commit_result> commit(operation_options operation = {}) &;
    task<stream_commit_result> commit(operation_options = {}) && = delete;

    /// 只在 commit 返回 body 后写正文；提交前、完成后调用均抛 logic_error。
    /// string_view 在调用返回前复制到 owner-worker PMR，临时正文可安全跨挂起。
    /// 输出缓冲及写超时作用域保留至传输操作完成。
    [[nodiscard]] task<void> write(std::span<const std::byte> chunk, operation_options operation = {}) &;
    task<void> write(std::span<const std::byte>, operation_options = {}) && = delete;
    [[nodiscard]] task<void> write(std::string_view chunk, operation_options operation = {}) &;
    task<void> write(std::string_view, operation_options = {}) && = delete;

    /// Takes ownership of an already-allocated chunk and transfers it into the
    /// output lane before returning.
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> write(string_type&& chunk, operation_options operation = {}) & {
        return write_owned(std::move(chunk), std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> write(string_type&&, operation_options = {}) && = delete;

    [[nodiscard]] task<void> writeln(std::string_view chunk, operation_options operation = {}) &;
    task<void> writeln(std::string_view, operation_options = {}) && = delete;

    /// 仅在正文阶段挂起生产者；正常超时返回 elapsed，context::stop_token()
    /// 或 `stop` 取消（含客户端断开）返回 stop_requested，与 sleep_for 相同：
    /// duration 是睡眠目标而非操作时限，故不接受 operation_options。
    /// 提交前或响应完成后调用抛 logic_error；HTTP/2 对端终止仍作为传输错误报告。
    [[nodiscard]] task<timer_sleep_result> sleep(timeout_duration duration, stop_token stop = {}) &;
    task<timer_sleep_result> sleep(timeout_duration, stop_token = {}) && = delete;

    /// 仅报告已观察到的真实失败或取消；正常完成不等于 aborted。
    /// 观察写失败与读侧终止（HTTP/1 请求体完整消费后的 FIN/RST，HTTP/2 流终止）。
    /// false 不保证对端仍连接。
    [[nodiscard]] bool aborted() const noexcept {
        return aborted_(target_);
    }
    [[nodiscard]] bool committed() const noexcept {
        return committed_(target_);
    }

    /// 正常结束正文并可附 Trailer；未提交时可用于提交一个空响应。
    /// Trailer 视图在调用返回前复制到 owner storage；HTTP/1.0 等无法承载的可选
    /// Trailer 会被省略，禁止 Trailer 的状态在未发送头之前拒绝非空部分。
    /// 已正常完成后空 end() 幂等，非空 Trailer 抛 logic_error；生产正文仍必须
    /// 先显式 commit() 并检查结果，不能用 end() 隐藏无正文阶段。
    [[nodiscard]] task<void> end(std::span<const http_header_view> trailers = {}, operation_options operation = {}) &;
    task<void> end(std::span<const http_header_view> = {}, operation_options = {}) && = delete;

private:
    friend struct detail::streaming_access;

    using commit_type = task<stream_commit_result> (*)(void*);
    using write_type = task<void> (*)(void*, std::string_view);
    using end_type = task<void> (*)(void*, std::span<const http_header_view>);
    using sleep_type = task<timer_sleep_result> (*)(void*, std::chrono::milliseconds, const stop_token&);
    using bind_context_type = void (*)(void*, context*);
    using release_context_type = void (*)(void*) noexcept;
    using committed_type = bool (*)(void*) noexcept;
    using aborted_type = bool (*)(void*) noexcept;

    // `interrupts` is the sink's; it routes stop requests to output operations.
    response_stream_writer(void* target, detail::operation_interrupt_source& interrupts,
        commit_type commit, write_type write, end_type end, sleep_type sleep, bind_context_type bind_context,
        release_context_type release_context, committed_type committed, aborted_type aborted) noexcept
        : target_(target),
          interrupts_(&interrupts),
          commit_(commit),
          write_(write),
          end_(end),
          sleep_(sleep),
          bind_context_(bind_context),
          release_context_(release_context),
          committed_(committed),
          aborted_(aborted) {}

    void bind_worker(const worker_ref& worker_value) noexcept {
        worker_ = &worker_value;
        operation_scope_.bind_worker(worker_value);
    }

    // The context's stop token travels with the binding so sleep() observes
    // worker shutdown, the client leaving, and explicit subrequest cancellation.
    // An interrupted output operation resets the request through the context.
    void bind_context(context& context_value, stop_token stop_token_value) {
        stop_token_ = std::move(stop_token_value);
        bind_context_(target_, &context_value);
        context_ = &context_value;
    }

    void release_context() noexcept {
        operation_scope_.close();
        release_context_(target_);
        context_ = nullptr;
    }

    void require_active() const {
        if (!operation_scope_.active()) {
            throw std::logic_error("response stream lifetime has expired");
        }
    }
    void require_output() const;

    task<void> write_owned(std::string&& chunk, operation_options operation);

    void* target_;
    detail::operation_interrupt_source* interrupts_;
    commit_type commit_;
    write_type write_;
    end_type end_;
    sleep_type sleep_;
    bind_context_type bind_context_;
    release_context_type release_context_;
    committed_type committed_;
    aborted_type aborted_;
    stop_token stop_token_;
    const worker_ref* worker_{nullptr};
    context* context_{nullptr};
    bool output_active_{false};
    ::ruvia::operation_scope operation_scope_;

    friend class sse_writer;
};

// A copyable handle to the context's response stream; operations borrow that
// stream, not this handle.
class sse_writer final {
public:
    sse_writer(const sse_writer& other) noexcept;
    sse_writer& operator=(const sse_writer&) = delete;
    sse_writer(sse_writer&& other) noexcept;
    sse_writer& operator=(sse_writer&&) = delete;

    /// 与 response_stream_writer 相同；completed 时停止事件生产，不伪造取消。
    [[nodiscard]] task<stream_commit_result> commit(operation_options operation = {}) &;
    task<stream_commit_result> commit(operation_options = {}) && = delete;
    [[nodiscard]] task<void> write(const sse_message& message, operation_options operation = {}) &;
    task<void> write(const sse_message&, operation_options = {}) && = delete;

    [[nodiscard]] task<timer_sleep_result> sleep(timeout_duration duration, stop_token stop = {}) &;
    task<timer_sleep_result> sleep(timeout_duration, stop_token = {}) && = delete;

    [[nodiscard]] bool aborted() const noexcept {
        return writer_ == nullptr || writer_->aborted();
    }
    [[nodiscard]] bool committed() const noexcept {
        return writer().committed();
    }

    [[nodiscard]] task<void> end(std::span<const http_header_view> trailers = {}, operation_options operation = {}) &;
    task<void> end(std::span<const http_header_view> = {}, operation_options = {}) && = delete;

private:
    friend class context;

    explicit sse_writer(response_stream_writer& writer) noexcept;
    [[nodiscard]] response_stream_writer& writer() const;
    static void expire_capability(void* target) noexcept;

    response_stream_writer* writer_;
    scoped_capability_registration registration_;
};

namespace detail {

struct streaming_access final {
    [[nodiscard]] static body_reader make_body_reader(void* target, body_reader::read_type::invoke_type read) noexcept {
        return body_reader(target, read);
    }

    [[nodiscard]] static response_stream_writer make_response_stream_writer(
        void* target, operation_interrupt_source& interrupts, response_stream_writer::commit_type commit,
        response_stream_writer::write_type write, response_stream_writer::end_type end,
        response_stream_writer::sleep_type sleep, response_stream_writer::bind_context_type bind_context,
        response_stream_writer::release_context_type release_context,
        response_stream_writer::committed_type committed, response_stream_writer::aborted_type aborted) noexcept {
        return response_stream_writer(
            target, interrupts, commit, write, end, sleep, bind_context, release_context, committed, aborted);
    }

    static void bind_worker(body_reader& reader, const worker_ref& worker_value) noexcept {
        reader.bind_worker(worker_value);
    }

    static void bind_worker(response_stream_writer& writer, const worker_ref& worker_value) noexcept {
        writer.bind_worker(worker_value);
    }

    [[nodiscard]] static bool& output_lane(response_stream_writer& writer) noexcept {
        return writer.output_active_;
    }

    static void bind_context(response_stream_writer& writer, context& context_value, stop_token stop_token_value) {
        writer.bind_context(context_value, std::move(stop_token_value));
    }

    static void release_context(response_stream_writer& writer) noexcept {
        writer.release_context();
    }
};

}  // namespace detail

class multipart_reader final {
public:
    multipart_reader(body_reader& body_reader_value, multipart_parse_options options)
        : body_reader_(nullptr),
          state_(std::in_place_type<expired_state_type>) {
        if (body_reader_value.operation_scope_.active()) {
            state_.emplace<receiving_state_type>(options);
            body_reader_ = &body_reader_value;
            if (body_reader_value.worker_ != nullptr) {
                operation_scope_.bind_worker(*body_reader_value.worker_);
            }
            registration_.bind(body_reader_value.operation_scope_, this, &multipart_reader::expire_capability);
        }
    }

    multipart_reader(const multipart_reader&) = delete;
    multipart_reader& operator=(const multipart_reader&) = delete;
    multipart_reader(multipart_reader&&) = delete;
    multipart_reader& operator=(multipart_reader&&) = delete;

    /// Returns one typed chunk of the current multipart part. All returned views
    /// remain valid only until the next read operation starts, parent body-reader
    /// expiry, or this reader's destruction. Unstarted read operations may coexist.
    /// `operation` bounds every body read this call makes, as body_reader::read()
    /// does one: a timeout or stop ends only this call and parsing resumes on the next.
    [[nodiscard]] task<std::optional<multipart_stream_part>> read(operation_options operation = {}) &;
    task<std::optional<multipart_stream_part>> read(operation_options = {}) && = delete;

private:
    struct receiving_state_type final {
        explicit receiving_state_type(multipart_parse_options options)
            : parser_(options) {}

        multipart_parser parser_;
    };
    struct finished_state_type final {};
    struct failed_state_type final {};
    struct expired_state_type final {};

    using state_type = std::variant<receiving_state_type, finished_state_type, failed_state_type, expired_state_type>;

    class read_guard_type final {
    public:
        explicit read_guard_type(multipart_reader& reader_value) noexcept
            : reader_(reader_value) {}

        ~read_guard_type() {
            if (!committed_ && std::holds_alternative<receiving_state_type>(reader_.state_)) {
                reader_.state_.emplace<failed_state_type>();
            }
        }

        void commit() noexcept {
            committed_ = true;
        }

    private:
        multipart_reader& reader_;
        bool committed_{false};
    };

    [[nodiscard]] body_reader& get_body_reader() const;
    static void expire_capability(void* target) noexcept;

    body_reader* body_reader_;
    state_type state_;
    bool read_active_{false};
    ::ruvia::operation_scope operation_scope_;
    scoped_capability_registration registration_;

    [[nodiscard]] task<std::optional<multipart_stream_part>> read_task(operation_options operation);
};

namespace detail {
class http_capsule_stream_state;
class capsule_state_pin;
class inbound_buffer_account;
template <typename stream_type>
class http_tunnel_session;
// Transport input for a single tunnel read lane. Datagram bytes exclude the
// HTTP/3 Quarter Stream ID but retain any protocol-specific Context ID.
struct http_tunnel_input final {
    std::string bytes_;
    bool datagram_{};
};
}  // namespace detail

class http_tunnel;
class http_client_tunnel;
class http_udp_tunnel;
class http_datagram_stream;

// Owns one capsule payload. Its storage remains valid across subsequent reads,
// stream destruction and connection retirement, even if input remains unread.
// Like other worker results, destroy it on its owner worker before that worker retires.
class http_capsule final {
public:
    http_capsule(const http_capsule&) = delete;
    http_capsule& operator=(const http_capsule&) = delete;
    http_capsule(http_capsule&& other) noexcept;
    http_capsule& operator=(http_capsule&& other) noexcept;
    ~http_capsule();
    [[nodiscard]] std::uint64_t type() const noexcept {
        return type_;
    }
    [[nodiscard]] std::string_view payload() const& noexcept RUVIA_LIFETIMEBOUND {
        return payload_ ? std::string_view(*payload_) : std::string_view{};
    }
    std::string_view payload() const&& = delete;
    [[nodiscard]] std::span<const std::byte> bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        const auto view = payload();
        return std::as_bytes(std::span(view.data(), view.size()));
    }
    std::span<const std::byte> bytes() const&& = delete;
    [[nodiscard]] std::string_view text() const& noexcept RUVIA_LIFETIMEBOUND {
        return payload();
    }
    std::string_view text() const&& = delete;

private:
    friend class http_capsule_stream;
    friend class http_datagram_stream;
    http_capsule(detail::http_capsule_stream_state& state_value, std::uint64_t type, std::string payload_value) noexcept;
    void release() noexcept;
    detail::http_capsule_stream_state* state_{};
    std::uint64_t type_{};
    std::optional<std::string> payload_;
};

// RFC 9297 reliable Capsule Protocol over an established tunnel. The protocol
// using the capsules owns negotiation. Server streams borrow their route
// tunnel; client streams take tunnel ownership. Use this adapter exclusively:
// do not interleave raw tunnel operations with capsule operations. Abort wakes
// pending I/O; join started operations before destroying their scope owner.
// Operations occupy their read or write/finish lane when started; unstarted
// operations may coexist. One read and one output operation may run concurrently.
// `operation` bounds a whole call: a timeout or stop ends only that read,
// keeping a partly received capsule for the next one, and aborts the stream
// during an output operation. Either throws std::system_error (timed_out or
// operation_canceled) -- over an http_client_tunnel its http_client_error
// subclass -- and a stop requested before an output operation starts sends nothing.
class http_capsule_stream final {
public:
    http_capsule_stream(const http_capsule_stream&) = delete;
    http_capsule_stream& operator=(const http_capsule_stream&) = delete;
    http_capsule_stream(http_capsule_stream&& other) noexcept;
    http_capsule_stream& operator=(http_capsule_stream&& other) noexcept;
    ~http_capsule_stream();
    [[nodiscard]] task<std::optional<http_capsule>> read(operation_options operation = {}) &;
    task<std::optional<http_capsule>> read(operation_options = {}) && = delete;
    // Owns input before returning its cold operation. Unknown capsule types are
    // delivered to the protocol consumer, which must ignore types it does not understand.
    [[nodiscard]] task<void> write(std::uint64_t type, std::string_view payload, operation_options operation = {}) &;
    task<void> write(std::uint64_t, std::string_view, operation_options = {}) && = delete;
    [[nodiscard]] task<void> write(std::uint64_t type, std::span<const std::byte> payload, operation_options operation = {}) &;
    task<void> write(std::uint64_t, std::span<const std::byte>, operation_options = {}) && = delete;
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> write(std::uint64_t type, string_type&& payload, operation_options operation = {}) & {
        return write_owned(type, std::move(payload), std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> write(std::uint64_t, string_type&&, operation_options = {}) && = delete;
    [[nodiscard]] task<void> finish(operation_options operation = {}) &;
    task<void> finish(operation_options = {}) && = delete;
    void abort() & noexcept;
    void abort() && = delete;

private:
    friend class http_tunnel;
    friend class http_client_tunnel;
    friend class http_udp_tunnel;
    friend class http_datagram_stream;
    [[nodiscard]] static task<std::optional<http_capsule>> read_owned(detail::capsule_state_pin pin, operation_options operation);
    [[nodiscard]] task<void> write_frame(std::string_view prefix, std::string_view payload, bool datagram, operation_options operation);
    [[nodiscard]] task<void> write_owned(std::uint64_t type, std::string&& payload, operation_options operation);
    [[nodiscard]] task<void> write_frame_owned(
        std::string_view prefix, std::string&& payload, bool datagram, operation_options operation);
    explicit http_capsule_stream(http_tunnel& tunnel, http_capsule_config config);
    explicit http_capsule_stream(http_client_tunnel&& tunnel, http_capsule_config config);
    void release() noexcept;
    detail::http_capsule_stream_state* state_{};
};

// Owned opaque HTTP Datagram payload, valid across later reads, stream and
// connection retirement, and client shutdown even if input remains unread.
// Destroy results on their worker before the worker retires.
class http_datagram final {
public:
    http_datagram(const http_datagram&) = delete;
    http_datagram& operator=(const http_datagram&) = delete;
    http_datagram(http_datagram&&) noexcept = default;
    http_datagram& operator=(http_datagram&&) noexcept = default;
    [[nodiscard]] std::span<const std::byte> payload() const& noexcept RUVIA_LIFETIMEBOUND {
        const auto view = text();
        return std::as_bytes(std::span(view.data(), view.size()));
    }
    std::span<const std::byte> payload() const&& = delete;
    [[nodiscard]] std::span<const std::byte> bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        return payload();
    }
    std::span<const std::byte> bytes() const&& = delete;
    [[nodiscard]] std::string_view text() const& noexcept RUVIA_LIFETIMEBOUND {
        auto bytes_value = capsule_.payload();
        bytes_value.remove_prefix(std::min(offset_, bytes_value.size()));
        return bytes_value;
    }
    std::string_view text() const&& = delete;

private:
    friend class http_datagram_stream;
    http_datagram(http_capsule capsule, std::size_t offset) noexcept
        : capsule_(std::move(capsule)),
          offset_(offset) {}
    http_capsule capsule_;
    std::size_t offset_{};
};

// RFC 9297 channel using native QUIC DATAGRAMs when available, with reliable
// DATAGRAM capsule fallback. Reads accept both representations concurrently.
// Create through http_tunnel::datagrams() or http_client_tunnel::datagrams();
// arbitrary Capsule Protocol streams cannot be promoted to this channel.
// Empty payloads are data; an empty optional is EOF. Use exclusively with the tunnel.
// Operations occupy separate read and send/finish lanes when started; unstarted
// operations may coexist. send() owns its payload before returning. `operation`
// follows http_capsule_stream.
class http_datagram_stream final {
public:
    http_datagram_stream(const http_datagram_stream&) = delete;
    http_datagram_stream& operator=(const http_datagram_stream&) = delete;
    http_datagram_stream(http_datagram_stream&&) noexcept = default;
    http_datagram_stream& operator=(http_datagram_stream&&) noexcept = default;
    [[nodiscard]] task<std::optional<http_datagram>> read(operation_options operation = {}) &;
    task<std::optional<http_datagram>> read(operation_options = {}) && = delete;
    [[nodiscard]] task<void> send(std::span<const std::byte> payload, operation_options operation = {}) &;
    task<void> send(std::span<const std::byte>, operation_options = {}) && = delete;
    [[nodiscard]] task<void> send(std::string_view payload, operation_options operation = {}) &;
    task<void> send(std::string_view, operation_options = {}) && = delete;
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> send(string_type&& payload, operation_options operation = {}) & {
        return send_owned(std::move(payload), std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> send(string_type&&, operation_options = {}) && = delete;
    [[nodiscard]] task<void> finish(operation_options operation = {}) & {
        return stream_.finish(std::move(operation));
    }
    task<void> finish(operation_options = {}) && = delete;
    void abort() & noexcept {
        stream_.abort();
    }
    void abort() && = delete;

private:
    friend class http_tunnel;
    friend class http_client_tunnel;
    friend class http_udp_tunnel;
    explicit http_datagram_stream(http_capsule_stream&& stream);
    static task<std::optional<http_datagram>> read_owned(detail::capsule_state_pin pin, bool udp, operation_options operation);
    [[nodiscard]] task<void> send_payload(std::string_view payload, bool udp, operation_options operation);
    [[nodiscard]] task<void> send_owned(std::string&& payload, operation_options operation);
    [[nodiscard]] task<void> send_payload_owned(
        std::string&& payload, bool udp, operation_options operation);
    http_capsule_stream stream_;
};

// RFC 9297 HTTP Datagrams and reliable DATAGRAM capsules for a negotiated RFC 9298 CONNECT-UDP
// tunnel. Unknown capsule types and unknown context IDs are silently dropped;
// malformed DATAGRAM capsules terminate only this tunnel. Empty UDP packets are
// data; an empty optional is EOF. Owns its capsule stream and preserves separate
// read and send/finish lanes when moved.
// Operations occupy their lane when started; unstarted operations may coexist.
// send() owns its payload before returning. `operation` follows http_capsule_stream.
class http_udp_tunnel final {
public:
    http_udp_tunnel(const http_udp_tunnel&) = delete;
    http_udp_tunnel& operator=(const http_udp_tunnel&) = delete;
    http_udp_tunnel(http_udp_tunnel&&) noexcept = default;
    http_udp_tunnel& operator=(http_udp_tunnel&&) noexcept = default;
    [[nodiscard]] task<std::optional<http_datagram>> read(operation_options operation = {}) &;
    task<std::optional<http_datagram>> read(operation_options = {}) && = delete;
    [[nodiscard]] task<void> send(std::span<const std::byte> payload, operation_options operation = {}) &;
    task<void> send(std::span<const std::byte>, operation_options = {}) && = delete;
    [[nodiscard]] task<void> send(std::string_view payload, operation_options operation = {}) &;
    task<void> send(std::string_view, operation_options = {}) && = delete;
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> send(string_type&& payload, operation_options operation = {}) & {
        return send_owned(std::move(payload), std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> send(string_type&&, operation_options = {}) && = delete;
    [[nodiscard]] task<void> finish(operation_options operation = {}) & {
        return stream_.finish(std::move(operation));
    }
    task<void> finish(operation_options = {}) && = delete;
    void abort() & noexcept {
        stream_.abort();
    }
    void abort() && = delete;

private:
    friend class http_tunnel;
    friend class http_client_tunnel;
    [[nodiscard]] task<void> send_owned(std::string&& payload, operation_options operation);
    explicit http_udp_tunnel(http_datagram_stream stream) noexcept
        : stream_(std::move(stream)) {}
    http_datagram_stream stream_;
};
struct http_tunnel_route_config final {
    // Once the handler returns, bound flushing the local FIN and draining peer
    // input. A handler may half-close and continue reading without this deadline.
    timeout_duration peer_transport_fin_timeout_{5000};
    // Declares HTTP Datagram semantics for an upgrade-protocol
    // tunnel: a 2xx response carries Capsule-Protocol: ?1 (RFC 9297 section
    // 3.4) and the handler may use http_tunnel::datagrams(), udp() and
    // capsules(). HTTP/3 datagrams use native transport when negotiated and
    // within the path limit, otherwise capsules. CONNECT-UDP enables this
    // automatically; plain CONNECT rejects it.
    bool datagrams_{};
};

// An established CONNECT byte stream. The session owns this facade and its
// allocator. The tunnel retains the last received chunk until the next read
// starts; it counts against the connection's and the worker's inbound buffer
// budgets (server_config::max_inbound_buffer_bytes_per_connection_ /
// _per_worker_). A read that would exceed a budget throws std::bad_alloc.
class http_tunnel final {
public:
    http_tunnel(const http_tunnel&) = delete;
    http_tunnel& operator=(const http_tunnel&) = delete;

    // One running read and one running output operation are allowed.
    // An operation occupies its lane when started; unstarted operations may coexist.
    // A timeout or stop in a read's `operation` ends only that read with
    // std::system_error (timed_out or operation_canceled); no input is lost.
    // In a write or finish it aborts the tunnel (a partial write cannot be
    // withdrawn) and throws the same; a stop requested before the operation
    // starts sends nothing. A non-positive timeout throws std::invalid_argument.
    // The view borrows this tunnel until the next read starts; nullopt is EOF
    // and an empty chunk is never returned.
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read(operation_options operation = {}) &;
    task<std::optional<std::span<const std::byte>>> read(operation_options = {}) && = delete;
    [[nodiscard]] task<void> write(std::string_view bytes, operation_options operation = {}) &;
    task<void> write(std::string_view, operation_options = {}) && = delete;
    [[nodiscard]] task<void> write(std::span<const std::byte> bytes, operation_options operation = {}) &;
    task<void> write(std::span<const std::byte>, operation_options = {}) && = delete;
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> write(string_type&& bytes, operation_options operation = {}) & {
        return write_owned(std::move(bytes), std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> write(string_type&&, operation_options = {}) && = delete;

    // Half-close the send direction; reading remains available until peer EOF.
    [[nodiscard]] task<void> finish(operation_options operation = {}) &;
    task<void> finish(operation_options = {}) && = delete;
    void abort() & noexcept;
    void abort() && = delete;
    // Borrow the established route tunnel for HTTP Datagrams. The route protocol
    // owns negotiation: these throw std::logic_error unless the route declared
    // http_tunnel_route_config::datagrams_. Use only in the negotiated datagram
    // handler, exclusively with this tunnel. Capsule Protocol payload limits
    // apply to both native datagrams and reliable DATAGRAM capsules.
    [[nodiscard]] http_datagram_stream datagrams(http_capsule_config config = {}) &;
    http_datagram_stream datagrams(http_capsule_config = {}) && = delete;
    [[nodiscard]] http_udp_tunnel udp(http_capsule_config config = {}) &;
    http_udp_tunnel udp(http_capsule_config = {}) && = delete;
    [[nodiscard]] http_capsule_stream capsules(http_capsule_config config = {}) &;
    http_capsule_stream capsules(http_capsule_config = {}) && = delete;

private:
    template <typename stream_type>
    friend class detail::http_tunnel_session;
    friend class detail::http_capsule_stream_state;
    friend class http_capsule_stream;
    using read_type = task<std::optional<std::string>> (*)(void*, operation_options);
    using write_type = task<void> (*)(void*, std::string_view, operation_options);
    using read_datagram_type = task<std::optional<detail::http_tunnel_input>> (*)(void*, operation_options);
    using write_datagram_type = task<bool> (*)(void*, std::string_view, operation_options);
    using finish_type = task<void> (*)(void*, operation_options);
    // malformed: the received tunnel data is malformed (RFC 9297 section 3.3).
    using abort_type = void (*)(void*, bool malformed) noexcept;
    http_tunnel(detail::inbound_buffer_account* inbound, const worker_ref& worker_value,
        void* target, read_type read, write_type write, finish_type finish_value, abort_type abort,
        bool datagrams, read_datagram_type read_datagram = nullptr,
        write_datagram_type write_datagram = nullptr) noexcept;
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read_borrowed(operation_options operation);
    [[nodiscard]] task<std::optional<std::string>> read_raw(operation_options operation);
    [[nodiscard]] task<void> write_owned(std::string&& bytes, operation_options operation);
    [[nodiscard]] task<void> write_raw(std::string&& bytes, operation_options operation);
    [[nodiscard]] task<std::optional<detail::http_tunnel_input>> read_datagram_raw(operation_options operation);
    [[nodiscard]] task<bool> write_datagram_raw(std::string_view bytes, operation_options operation);
    void abort_input(bool malformed) noexcept;
    void require_active() const;
    void require_unadapted() const;
    detail::inbound_buffer_account* inbound_;
    const worker_ref& worker_;
    void* target_;
    read_type read_;
    write_type write_;
    finish_type finish_;
    abort_type abort_;
    read_datagram_type read_datagram_;
    write_datagram_type write_datagram_;
    // The chunk the last read() returned.
    std::optional<std::string> read_chunk_;
    bool datagrams_;
    bool adapted_{false};
    bool read_active_{false};
    bool write_active_{false};
    bool send_ended_{false};
    ::ruvia::operation_scope operations_;
};

struct websocket_heartbeat_config final {
    // Absence disables heartbeat. When ping is present and pong is absent,
    // pong defaults to the ping interval during configuration normalization.
    std::optional<timeout_duration> ping_interval_{};
    std::optional<timeout_duration> pong_timeout_{};
};

// Runtime-only liveness policy. Wire framing and close-handshake state remain in
// ruvia-http; timers and transport abort policy belong to the Web runtime.
struct websocket_lifecycle_options final {
    websocket_heartbeat_config heartbeat_{};
    // A locally initiated Close waits for the peer Close before the underlying
    // transport is ended. The timeout starts when the Close is submitted, so it
    // also bounds writing the Close frame. nullopt disables this guard.
    std::optional<timeout_duration> close_handshake_timeout_{std::chrono::seconds(5)};
};

struct websocket_route_config final {
    // Server preference order. Every entry must be a nonempty, unique HTTP token.
    std::vector<std::string> subprotocols_{};
    // By default a client offering none of subprotocols_ is still accepted,
    // with no Sec-WebSocket-Protocol response field (RFC 6455 4.2.2). When
    // set, such an upgrade is refused with 400 before any middleware or
    // handler runs; subprotocols_ must then be nonempty.
    bool require_subprotocol_{false};
    websocket_lifecycle_options lifecycle_{};
    websocket_deflate_config deflate_{};
    // 单会话接收消息上限（含解压后）；unlimited 继承服务器上限，只能收紧。
    protocol_byte_limit max_message_bytes_{};
};

struct websocket_send_options final {
    // False bypasses compression and never adds payload bytes to its dictionary.
    bool compress_{true};
};

struct websocket_close_options final {
    websocket_close_code code_{websocket_close_status::normal_closure};
    ::ruvia::borrowed_text reason_{};
};

class websocket;

namespace detail {
template <typename connection_type>
[[nodiscard]] websocket make_websocket_facade(connection_type& connection);
struct websocket_accept_access;
class websocket_state;
// Whether a write submits a whole message or one fragment of one.
enum class websocket_write_extent : std::uint8_t {
    message,
    fragment,
    final_fragment,
};
}  // namespace detail

class websocket_message_writer;

// A handshake the server refused. The server sends the selected response,
// with this status, after the request callback returns.
struct websocket_rejection final {
    http_status_code status_;
    // The failed handshake check, as in the error response's code:
    // invalid_websocket_handshake, unsupported_websocket_version or
    // unsupported_websocket_subprotocol.
    std::string_view code_;
};

// Outcome of accepting a WebSocket upgrade, fixed at construction: the
// established session, borrowed until the request callback ends, or the
// rejection.
class websocket_accept_result final {
public:
    websocket_accept_result(const websocket_accept_result&) = delete;
    websocket_accept_result& operator=(const websocket_accept_result&) = delete;
    websocket_accept_result(websocket_accept_result&&) noexcept = default;
    websocket_accept_result& operator=(websocket_accept_result&&) = delete;

    // nullptr when the handshake was rejected.
    [[nodiscard]] websocket* session() const noexcept {
        const auto* accepted = std::get_if<websocket*>(&value_);
        return accepted != nullptr ? *accepted : nullptr;
    }
    // nullptr when the session was established.
    [[nodiscard]] const websocket_rejection* rejection() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<websocket_rejection>(&value_);
    }
    const websocket_rejection* rejection() const&& = delete;

private:
    friend struct detail::websocket_accept_access;
    explicit websocket_accept_result(websocket& session) noexcept
        : value_(&session) {}
    explicit websocket_accept_result(websocket_rejection rejection) noexcept
        : value_(rejection) {}
    std::variant<websocket*, websocket_rejection> value_;
};

// One worker-affine session API for both roles. Client handshakes return an owning
// handle; server acceptance borrows the request lifetime. Moving preserves writers
// and cold/running operations, but never extends a server request lifetime.
class websocket final {
public:
    websocket(const websocket&) = delete;
    websocket& operator=(const websocket&) = delete;
    websocket(websocket&&) noexcept;
    websocket& operator=(websocket&&) noexcept;
    ~websocket();

    /// Only one read operation may run at a time. Starting another before the
    /// current operation completes throws std::logic_error. Unstarted operations may coexist.
    /// One reader may overlap one writer. A reader waiting behind an in-flight
    /// message polls control frames at message boundaries before the writer's
    /// next operation; the current complete message is never interrupted.
    /// nullopt is the only end: the peer's or this side's completed close
    /// handshake, EOF, a rejected frame, abort() or a heartbeat/close timeout;
    /// termination() tells which. A stop request or timeout in `operation`
    /// ends only this read with std::system_error (operation_canceled or
    /// timed_out); buffered input, the read lane and the session stay usable.
    [[nodiscard]] task<std::optional<websocket_message>> read(operation_options operation = {}) &;
    task<std::optional<websocket_message>> read(operation_options = {}) && = delete;

    /// The string_view overloads copy payloads into owned storage before
    /// returning. std::string&& inputs transfer their existing allocation.
    /// Output operations share one write lane: starting a second before the
    /// current operation completes throws std::logic_error. A stop request or
    /// timeout in `operation` aborts the session (a partly written frame
    /// cannot be withdrawn) and throws std::system_error (operation_canceled
    /// or timed_out); an already-stopped token fails before any output.
    [[nodiscard]] task<void> text(std::string_view payload, websocket_send_options options = {}, operation_options operation = {}) &;
    task<void> text(std::string_view, websocket_send_options = {}, operation_options = {}) && = delete;

    /// Zero-copy text frame: takes ownership of an already-allocated payload.
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> text(string_type&& payload, websocket_send_options options = {}, operation_options operation = {}) & {
        return write_owned(websocket_opcode::text, std::move(payload), options.compress_, std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> text(string_type&&, websocket_send_options = {}, operation_options = {}) && = delete;

    [[nodiscard]] task<void> binary(std::string_view payload, websocket_send_options options = {}, operation_options operation = {}) &;
    task<void> binary(std::string_view, websocket_send_options = {}, operation_options = {}) && = delete;

    [[nodiscard]] task<void> binary(std::span<const std::byte> payload, websocket_send_options options = {}, operation_options operation = {}) &;
    task<void> binary(std::span<const std::byte>, websocket_send_options = {}, operation_options = {}) && = delete;

    /// Zero-copy binary frame.
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> binary(string_type&& payload, websocket_send_options options = {}, operation_options operation = {}) & {
        return write_owned(websocket_opcode::binary, std::move(payload), options.compress_, std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> binary(string_type&&, websocket_send_options = {}, operation_options = {}) && = delete;

    [[nodiscard]] task<void> pong(std::string_view payload = {}, operation_options operation = {}) &;
    task<void> pong(std::string_view = {}, operation_options = {}) && = delete;

    /// Zero-copy pong frame.
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> pong(string_type&& payload, operation_options operation = {}) & {
        return write_owned(websocket_opcode::pong, std::move(payload), true, std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> pong(string_type&&, operation_options = {}) && = delete;

    [[nodiscard]] task<void> ping(std::string_view payload = {}, operation_options operation = {}) &;
    task<void> ping(std::string_view = {}, operation_options = {}) && = delete;

    /// Zero-copy ping frame.
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> ping(string_type&& payload, operation_options operation = {}) & {
        return write_owned(websocket_opcode::ping, std::move(payload), true, std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> ping(string_type&&, operation_options = {}) && = delete;

    /// Zero-copy send of a message returned by this connection's read(), with
    /// the message's own opcode (for example an echo). The frame borrows the
    /// payload from the connection's read storage, so this call claims the
    /// read lane immediately and holds it until the operation completes or is
    /// destroyed: read() and close() throw std::logic_error meanwhile, and this
    /// call throws std::logic_error while a read is pending. A message built
    /// with websocket_message::borrow over caller storage must keep that
    /// storage alive and unchanged until the operation completes.
    [[nodiscard]] task<void> send(websocket_message&& message, websocket_send_options options = {}, operation_options operation = {}) &;
    task<void> send(websocket_message&&, websocket_send_options = {}, operation_options = {}) && = delete;

    /// Claims the write lane only, so it may start while a read is pending:
    /// that read returns nullopt once the peer answers (termination()
    /// local_close) or the close-handshake timeout aborts the transport
    /// (timeout). Without a pending read, close() itself reads until the peer
    /// Close and a read started meanwhile throws std::logic_error. A handshake
    /// timeout throws std::system_error (timed_out); `operation` bounds the
    /// whole close like an output operation.
    [[nodiscard]] task<void> close(websocket_close_options options = {}, operation_options operation = {}) &;
    task<void> close(websocket_close_options = {}, operation_options = {}) && = delete;
    void abort() & noexcept;
    void abort() && = delete;

    /// Starts a data message sent as several frames; see websocket_message_writer.
    /// Throws std::logic_error while another such message is open.
    [[nodiscard]] websocket_message_writer begin_text(websocket_send_options options = {}) &;
    websocket_message_writer begin_text(websocket_send_options = {}) && = delete;
    [[nodiscard]] websocket_message_writer begin_binary(websocket_send_options options = {}) &;
    websocket_message_writer begin_binary(websocket_send_options = {}) && = delete;

    [[nodiscard]] std::string_view subprotocol() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view subprotocol() const&& = delete;

    [[nodiscard]] std::optional<std::uint16_t> peer_close_code() const noexcept;
    [[nodiscard]] std::string_view peer_close_reason() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view peer_close_reason() const&& = delete;

    /// Why the session ended; none while it is active. The first cause wins
    /// and stays readable once read() has returned nullopt.
    [[nodiscard]] websocket_termination termination() const noexcept;
    /// RFC 6455 frame bytes on the wire (header, mask key and payload, after
    /// permessage-deflate) of data and control frames; the HTTP upgrade and
    /// the HTTP/2 or HTTP/3 framing that carries the frames are excluded.
    [[nodiscard]] std::uint64_t bytes_received() const noexcept;
    [[nodiscard]] std::uint64_t bytes_sent() const noexcept;

    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] const worker_ref& worker() const& RUVIA_LIFETIMEBOUND;
    const worker_ref& worker() const&& = delete;

private:
    friend class http_client_websocket_request;
    template <typename connection_type>
    friend websocket detail::make_websocket_facade(connection_type&);
    explicit websocket(std::shared_ptr<detail::websocket_state> state) noexcept;
    [[nodiscard]] task<void> write_owned(websocket_opcode, std::string&&, bool, operation_options);
    std::shared_ptr<detail::websocket_state> state_;
};

/// A data message sent as several frames (RFC 6455 section 5.4), so that a
/// caller can pace a large message, for example under a shared bandwidth
/// budget. Each write() sends one non-final frame and completes once the
/// transport accepted it; finish() sends the final frame. Fragments of a
/// compressed message continue one permessage-deflate stream. Until finish()
/// completes the message owns the data lane: text(), binary(), send() and
/// another begin_*() throw std::logic_error, while ping(), pong(), close() and
/// automatic control frames may go out between fragments. Fragment operations
/// share the write lane with them and accept the same operation_options.
/// Destroying or abort()ing an unfinished message aborts the session, since
/// the peer could not tell where the message ends. The writer borrows the
/// websocket and expires with it.
class websocket_message_writer final {
public:
    websocket_message_writer(websocket_message_writer&& other) noexcept;
    websocket_message_writer& operator=(websocket_message_writer&&) = delete;
    websocket_message_writer(const websocket_message_writer&) = delete;
    websocket_message_writer& operator=(const websocket_message_writer&) = delete;
    ~websocket_message_writer();

    [[nodiscard]] task<void> write(std::string_view chunk, operation_options operation = {}) &;
    task<void> write(std::string_view, operation_options = {}) && = delete;
    [[nodiscard]] task<void> write(std::span<const std::byte> chunk, operation_options operation = {}) &;
    task<void> write(std::span<const std::byte>, operation_options = {}) && = delete;
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> write(string_type&& chunk, operation_options operation = {}) & {
        return submit_owned(std::move(chunk), false, std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> write(string_type&&, operation_options = {}) && = delete;
    [[nodiscard]] task<void> finish(std::string_view last_chunk = {}, operation_options operation = {}) &;
    task<void> finish(std::string_view = {}, operation_options = {}) && = delete;
    [[nodiscard]] task<void> finish(std::span<const std::byte> last_chunk, operation_options operation = {}) &;
    task<void> finish(std::span<const std::byte>, operation_options = {}) && = delete;
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> finish(string_type&& last_chunk, operation_options operation = {}) & {
        return submit_owned(std::move(last_chunk), true, std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> finish(string_type&&, operation_options = {}) && = delete;
    void abort() & noexcept;
    void abort() && = delete;

private:
    friend class detail::websocket_state;
    websocket_message_writer(detail::websocket_state& owner, websocket_opcode opcode, bool compress) noexcept;
    [[nodiscard]] task<void> submit(std::string_view chunk, bool final, operation_options operation);
    [[nodiscard]] task<void> submit_owned(
        std::string&& chunk, bool final, operation_options operation);
    [[nodiscard]] bool message_open() const noexcept;
    static void expire_capability(void* target) noexcept;

    detail::websocket_state* owner_;
    std::uint64_t message_id_;
    websocket_opcode opcode_;
    bool compress_;
    scoped_capability_registration registration_;
};

class env;
class static_root;
// Capabilities a handler reaches through context; include the defining header
// where they are used: context.h and http_client.h.
class http_client_response;
class test_app;
class tls_client_hello_record;
class server_connection;
#ifdef RUVIA_ENABLE_DATABASE
class db_session;
#endif
#ifdef RUVIA_ENABLE_REDIS
class redis_session;
#endif
namespace detail {
class context_request_storage;
class context_request_body_source;
class context_response_output;
class context_response_state;
template <typename t_type>
[[nodiscard]] request_binding_guard<t_type> bind_validated_model(context& context_value, const t_type& model);
template <typename t_type>
    requires(!std::is_lvalue_reference_v<t_type>)
request_binding_guard<std::remove_cvref_t<t_type>> bind_validated_model(context&, t_type&&) = delete;

[[noreturn]] void throw_invalid_query();
[[noreturn]] void throw_invalid_param();

struct context_access;
class context_services;
}  // namespace detail

class plain_connection_transport final {
private:
    friend class conn_info;
    constexpr plain_connection_transport() noexcept = default;
};

class tls_connection_transport final {
public:
    // Verified client certificate subject DN. Absence means no certificate;
    // an engaged empty value is a certificate with an empty Subject (RFC 5280
    // section 4.1.2.6), not an unauthenticated connection.
    [[nodiscard]] constexpr std::optional<std::string_view> client_certificate_subject() const noexcept {
        return client_certificate_subject_;
    }

    // The application protocol the handshake selected (RFC 7301); empty when
    // the client offered no ALPN.
    [[nodiscard]] constexpr std::string_view alpn() const noexcept {
        return alpn_;
    }

    // The host_name the client sent in server_name (RFC 6066 §3); empty when
    // it sent none.
    [[nodiscard]] constexpr std::string_view server_name() const noexcept {
        return server_name_;
    }

private:
    friend class conn_info;
    friend class server_connection;

    constexpr tls_connection_transport(std::optional<std::string_view> client_certificate_subject,
        std::string_view alpn, std::string_view server_name) noexcept
        : client_certificate_subject_(client_certificate_subject),
          alpn_(alpn),
          server_name_(server_name) {}

    std::optional<std::string_view> client_certificate_subject_;
    std::string_view alpn_;
    std::string_view server_name_;
};

// Runtime connection metadata associated with a Web request. This information
// comes from the server adapter rather than from the HTTP message bytes, so it
// intentionally lives outside http_request and context_request.
class conn_info final {
public:
    // Process-wide unique transport ID across workers, hosts and restarts.
    // Every request and observation on one connection shares this ID; retired
    // IDs are never reused. Never zero for a served connection; zero for a
    // test_app request, which has no transport connection.
    [[nodiscard]] constexpr std::uint64_t connection_id() const noexcept {
        return id_;
    }

    // The peer at the other end of this socket. Behind a reverse proxy that is
    // the proxy, not the caller -- use client() for the caller.
    [[nodiscard]] constexpr ip_endpoint remote() const noexcept {
        return remote_;
    }

    [[nodiscard]] constexpr ip_endpoint local() const noexcept {
        return local_;
    }
    // Who the request is from. Equal to remote() unless the peer is a configured
    // trusted proxy AND it sent a forwarding header, in which case it is what
    // that header names. Never derived from an untrusted peer's headers: those
    // are attacker-controlled, so with no trusted proxy configured this is
    // always the direct peer.
    //
    // This, not remote(), is what rate limiting keys on and what an access log
    // should record.
    [[nodiscard]] constexpr ip_endpoint client() const noexcept {
        return client_;
    }

    // Whether client()/scheme() came from a forwarding header rather than from
    // the transport. False for every request whose peer is not trusted.
    [[nodiscard]] constexpr bool via_trusted_proxy() const noexcept {
        return via_trusted_proxy_;
    }

    // https when the request reached the client over TLS -- including TLS the
    // proxy terminated and reported with X-Forwarded-Proto -- and http
    // otherwise. tls() describes only THIS hop, so a Secure cookie or an HSTS
    // header must be decided from this instead: behind a TLS-terminating proxy
    // the server's own transport is plaintext while the client's is not.
    [[nodiscard]] constexpr http_scheme scheme() const noexcept {
        return scheme_;
    }

    [[nodiscard]] constexpr const plain_connection_transport* plain() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<plain_connection_transport>(&transport_);
    }
    const plain_connection_transport* plain() const&& = delete;

    [[nodiscard]] constexpr const tls_connection_transport* tls() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<tls_connection_transport>(&transport_);
    }
    const tls_connection_transport* tls() const&& = delete;

private:
    friend class detail::context_services;

    conn_info(std::uint64_t id, std::string_view remote_address, plain_connection_transport transport) noexcept
        : id_(id),
          remote_(ip_address::parse(remote_address).value_or(ip_address{}), 0),
          remote_text_(remote_address),
          client_(remote_),
          transport_(transport),
          scheme_(http_scheme::http) {}

    conn_info(std::uint64_t id, std::string_view remote_address, tls_connection_transport transport) noexcept
        : id_(id),
          remote_(ip_address::parse(remote_address).value_or(ip_address{}), 0),
          remote_text_(remote_address),
          client_(remote_),
          transport_(transport),
          scheme_(http_scheme::https) {}

    [[nodiscard]] static conn_info plain(std::uint64_t id, std::string_view remote_address) noexcept {
        return conn_info(id, remote_address, plain_connection_transport{});
    }

    [[nodiscard]] static conn_info tls(std::uint64_t id, std::string_view remote_address,
        std::optional<std::string_view> client_certificate_subject, std::string_view alpn,
        std::string_view server_name) noexcept {
        return conn_info(id, remote_address, tls_connection_transport(client_certificate_subject, alpn, server_name));
    }

    // Applied only after the peer has been matched against the configured
    // trusted set. An empty scheme leaves the transport-derived value.
    constexpr void apply_forwarded(
        const ip_address& client_address, std::optional<http_scheme> forwarded_scheme) noexcept {
        client_ = ip_endpoint(client_address, 0);
        via_trusted_proxy_ = true;
        if (forwarded_scheme.has_value()) {
            scheme_ = *forwarded_scheme;
        }
    }

    constexpr void set_remote_port(std::uint16_t port) noexcept {
        remote_ = ip_endpoint(remote_.address(), port);
        if (!via_trusted_proxy_) {
            client_ = remote_;
        }
    }
    void set_local(std::string_view address, std::uint16_t port) noexcept {
        local_ = ip_endpoint(ip_address::parse(address).value_or(ip_address{}), port);
    }

    std::uint64_t id_;
    ip_endpoint remote_;
    std::string_view remote_text_;
    ip_endpoint client_;
    ip_endpoint local_{};
    std::variant<plain_connection_transport, tls_connection_transport> transport_;
    http_scheme scheme_;
    bool via_trusted_proxy_{false};
};

static_assert(std::is_nothrow_copy_constructible_v<conn_info>);
static_assert(std::is_nothrow_move_constructible_v<conn_info>);
static_assert(std::is_nothrow_copy_assignable_v<conn_info>);
static_assert(std::is_nothrow_move_assignable_v<conn_info>);

// Inputs are copied before dispatch() returns its lazy scoped operation.
// Only origin-form targets and buffered request/response bodies are accepted.
// Cancellation combines `stop_token_` with the calling context's stop token;
// subrequest handlers have no total execution timeout.
struct dispatch_options final {
    borrowed_text method_{};
    borrowed_text target_{};
    borrowed_http_headers headers_{};
    borrowed_text body_{};
    stop_token stop_token_{};
};

// Owns its response metadata and body independently of the child subrequest.
class dispatch_response final {
public:
    [[nodiscard]] http_status_code status() const noexcept {
        return status_;
    }
    [[nodiscard]] std::string_view body() const& noexcept RUVIA_LIFETIMEBOUND {
        return body_;
    }
    std::string_view body() const&& = delete;
    [[nodiscard]] std::span<const http_header> headers() const& noexcept RUVIA_LIFETIMEBOUND {
        return headers_;
    }
    std::span<const http_header> headers() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& noexcept RUVIA_LIFETIMEBOUND;
    std::optional<std::string_view> header(std::string_view) const&& = delete;

private:
    friend class context;
    friend class test_app;
    dispatch_response() = default;
    void copy_head_from(const http_response& response);
    http_status_code status_{http_status::ok};
    std::vector<http_header> headers_;
    std::string body_;
};

template <typename t_type>
using request_state_binding = detail::request_binding_guard<t_type>;

// Explicit opt-in for zero-copy response bodies: the response borrows these
// bytes instead of copying them. The consteval constructors accept only
// constant expressions that refer to static storage, such as string literals
// and namespace-scope or static constexpr arrays and views; frame-local or
// runtime buffers do not compile. Every other string handed to a context body
// builder is copied into response-owned storage.
class static_text final {
public:
    // Keeps embedded NUL bytes and drops only the literal's terminator.
    template <std::size_t n>
    consteval explicit static_text(const char (&literal)[n]) noexcept
        : value_(literal, n > 0 && literal[n - 1] == '\0' ? n - 1 : n) {}

    consteval explicit static_text(std::string_view value) noexcept
        : value_(value) {}

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return value_;
    }

private:
    std::string_view value_;
};

struct signed_cookie_lookup_options final {
    borrowed_text name_{};
    borrowed_text secret_{};
};

// Borrow-only request facade. Returned fields and body views belong to context,
// not this by-value facade. Model validation belongs to route middleware.
class context_request final {
public:
    [[nodiscard]] std::string_view method() const noexcept;
    [[nodiscard]] http_known_method known_method() const noexcept;
    [[nodiscard]] std::string_view path() const noexcept;
    [[nodiscard]] std::string_view scheme() const noexcept;
    [[nodiscard]] std::string_view authority() const noexcept;
    [[nodiscard]] std::string_view target() const noexcept;
    [[nodiscard]] http_protocol_version protocol_version() const noexcept;
    [[nodiscard]] http_request_target_form target_form() const noexcept;
    [[nodiscard]] std::string_view route_path() const noexcept;
    // Initial RFC 9218 Priority fields.
    [[nodiscard]] http_priority priority() const noexcept;

    // Accept uses media ranges; language uses RFC 4647 basic filtering;
    // charset matches tokens or '*'; encoding also handles coding aliases and
    // the default acceptability of identity.
    enum class negotiable_field : std::uint8_t { media_type,
        language,
        encoding,
        charset };
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const;
    // Read after body completion to observe the complete terminal section.
    // Trailers never replace initial headers or change route selection.
    [[nodiscard]] std::span<const http_header> trailers() const noexcept;
    [[nodiscard]] std::optional<std::string_view> trailer(std::string_view name) const noexcept;
    // Client weights select a caller-owned supported value; server order breaks
    // ties. An absent field selects the first offer, no acceptable offer is nullopt.
    [[nodiscard]] std::optional<std::string_view> negotiate(
        negotiable_field field, std::span<const std::string_view> supported) const noexcept;
    [[nodiscard]] std::optional<std::string_view> negotiate(
        negotiable_field field, std::initializer_list<std::string_view> supported) const noexcept {
        return negotiate(field, std::span<const std::string_view>(supported.begin(), supported.size()));
    }
    [[nodiscard]] std::optional<std::string_view> query(std::string_view name) const;
    [[nodiscard]] std::span<const std::string_view> queries(std::string_view name) const;
    [[nodiscard]] std::optional<std::string_view> cookie(std::string_view name) const;
    [[nodiscard]] std::optional<std::string_view> param(std::string_view name) const;

    template <detail::parsed_integer t_type>
    [[nodiscard]] std::optional<t_type> query(std::string_view name) const;
    template <std::same_as<bool> t_type>
    [[nodiscard]] std::optional<bool> query(std::string_view name) const;
    template <detail::parsed_integer t_type>
    [[nodiscard]] std::optional<t_type> param(std::string_view name) const;
    template <std::same_as<bool> t_type>
    [[nodiscard]] std::optional<bool> param(std::string_view name) const;
    // Materialized once in request order, preserving duplicates. Header names
    // retain their semantic spelling; header get/count
    // and header_model binding compare names ASCII case-insensitively.
    [[nodiscard]] const request_name_value_list& header_fields() const;
    [[nodiscard]] const request_name_value_list& query_fields() const;
    [[nodiscard]] const request_name_value_list& cookie_fields() const;
    [[nodiscard]] const request_name_value_list& param_fields() const;
    [[nodiscard]] std::optional<std::string_view> signed_cookie(signed_cookie_lookup_options options) const;

    // Buffered views borrow context storage; read the media type with header("Content-Type").
    // `operation` bounds the wait for body bytes in each of these reads, and
    // in json_if(), form_if() and multipart(): a timeout or stop ends only
    // that call with std::system_error (timed_out or operation_canceled),
    // keeps what arrived, and a later call resumes; a stop requested before
    // the call starts throws operation_canceled. A non-positive timeout throws
    // std::invalid_argument. Server body timeouts still apply. This view is a
    // copyable handle: the operations borrow the context, not the view.
    [[nodiscard]] task<std::string_view> text(operation_options operation = {}) const;
    [[nodiscard]] task<std::span<const std::byte>> bytes(operation_options operation = {}) const;
    [[nodiscard]] task<void> discard_body(operation_options operation = {}) const;

    // Typed optional parsing with schema and field-rule validation. Only a
    // media-type mismatch yields nullopt; selected but malformed input throws
    // HTTP 400, and rule violations throw an http_error carrying the issues.
    template <typename t_type>
    [[nodiscard]] task<std::optional<t_type>> json_if(operation_options operation = {}) const;
    template <typename t_type>
    [[nodiscard]] task<std::optional<t_type>> form_if(operation_options operation = {}) const;
    template <typename t_type>
    [[nodiscard]] const t_type& validated() const;

    // Multipart remains a flat protocol representation, without dotted paths
    // or implicit array/group interpretation. Streaming is explicit-route only.
    // multipart() decodes a supported Content-Encoding; multipart_reader()
    // decodes none, so a coded body is refused with 415 and an invalid
    // Content-Encoding field with 400.
    [[nodiscard]] task<std::vector<multipart_part>> multipart(operation_options operation = {}) const;
    [[nodiscard]] ::ruvia::body_reader& body_reader() const;
    [[nodiscard]] ::ruvia::multipart_reader multipart_reader() const;

private:
    friend class context;
    explicit constexpr context_request(const context& context_value RUVIA_LIFETIMEBOUND) noexcept
        : context_(&context_value) {}

    template <typename t_type>
    [[nodiscard]] static std::optional<t_type> parse_typed(
        std::optional<std::string_view> raw, void (*fail)());

    const context* context_{nullptr};
};
struct redirect_response_options final {
    // URI reference: preserves existing %HH escapes and the first '#' separator;
    // subsequent '#' bytes are encoded as %23. CR and LF are rejected, as is a
    // '\' inside the authority of an absolute or "//" reference (browsers read
    // it as a path separator, so encoding it would change the target host).
    borrowed_text location_{};
    http_status_code status_{http_status::found};
};

struct set_cookie_options final {
    borrowed_text name_{};
    borrowed_text value_{};
    cookie_options attributes_{};
};

struct set_signed_cookie_options final {
    borrowed_text name_{};
    borrowed_text value_{};
    borrowed_text secret_{};
    cookie_options attributes_{};
};

struct delete_cookie_options final {
    borrowed_text name_{};
    cookie_options attributes_{};
};

// 显式选择草稿或已选响应的元数据编辑能力；不暴露可绕过提交检查的引用。
// 每次修改都检查尚未提交，保存 editor 也不能在提交后修改头。
class context_response_editor final {
public:
    void status(http_status_code status_code);
    void header(std::string_view name, std::string_view value, http_response_header_options options = {});
    void priority(http_priority_fields fields);
    void remove_header(std::string_view name);
    void set_cookie(set_cookie_options options);
    void set_signed_cookie(set_signed_cookie_options options);
    void delete_cookie(delete_cookie_options options);

private:
    friend class context;
    enum class target_type { draft,
        selected,
        policy };
    context_response_editor(context& owner, target_type target) noexcept
        : owner_(&owner),
          target_(target) {}
    [[nodiscard]] http_response& target() const;
    context* owner_;
    target_type target_;
};

// Read only by the static_file() call: the relative path is resolved and the
// content type copied into request storage before the task is returned.
struct static_file_response_options final {
    borrowed_text relative_path_{};
    borrowed_text content_type_{};
};

// The file context::file() serves, read only by that call: UTF-8 text, or a
// std::filesystem::path lvalue whose native string is used as is. Neither form
// builds a std::filesystem::path per request; file() copies the native form
// once before returning its task, NUL-terminating text (and widening it to
// UTF-16 on Windows). Like borrowed_text, it refuses owning temporaries, so a
// path assembled per request has to be named by the caller -- hoist it out of
// the handler when it is fixed.
class borrowed_file_path final {
public:
    constexpr borrowed_file_path() noexcept = default;

    template <typename text_type>
        requires std::constructible_from<borrowed_text, text_type&&>
    constexpr borrowed_file_path(text_type&& utf8) noexcept(std::is_nothrow_constructible_v<borrowed_text, text_type&&>)
        : text_(std::forward<text_type>(utf8)) {}

    borrowed_file_path(const std::filesystem::path& path) noexcept
        : native_(path.native()) {}

    borrowed_file_path(std::filesystem::path&&) = delete;
    borrowed_file_path(const std::filesystem::path&&) = delete;

    // The UTF-8 text form; empty when a std::filesystem::path is borrowed.
    [[nodiscard]] constexpr std::string_view text() const noexcept {
        return text_.view();
    }

    // A borrowed std::filesystem::path's NUL-terminated native string; empty
    // for the text form.
    [[nodiscard]] constexpr http_native_path_view_type native() const noexcept {
        return native_;
    }

private:
    borrowed_text text_{};
    http_native_path_view_type native_{};
};

// Read only by the file() call: the path and content type are copied before
// the task is returned.
struct file_response_options final {
    borrowed_file_path path_{};
    borrowed_text content_type_{};
};

class context final {
private:
    friend class context_request;
    friend class context_response_editor;
    friend class http_client_response;
    friend struct detail::context_access;
    friend struct detail::session_access;
    template <typename t_type>
    friend detail::request_binding_guard<t_type> detail::bind_validated_model(
        context& context_value, const t_type& model);

    context(const http_request& request, const detail::context_services& services);

    context(const http_request& request, std::string_view route_path,
        const std::string_view* param_names, const std::string_view* param_values,
        std::size_t param_count, std::uintptr_t route_rate_limit_scope,
        const detail::context_services& services);

public:
    // Sends a bodyless 1xx head before the final response. Fields are copied
    // before return; the operation and its storage retire on the owning worker.
    // Output is linear when started. Unstarted operations may coexist and do not
    // block the final response; starting one after the final head commits throws.
    // A failed write throws std::system_error, which is not routed through
    // on_error: the connection ends. A timeout or stop in `operation` resets
    // the request (HTTP/2, HTTP/3: the stream; HTTP/1: the connection) and
    // throws std::system_error (timed_out or operation_canceled); a stop
    // requested before the operation starts sends nothing.
    [[nodiscard]] task<void> inform(const http_interim_response_head& response, operation_options operation = {}) &;
    task<void> inform(const http_interim_response_head&, operation_options = {}) && = delete;

    // Queues a promised request on this HTTP/2 connection. False means the
    // peer disabled push or its stream capacity cannot admit the promise.
    [[nodiscard]] bool push(http_push_request_view request);
    // Connection-scoped ORIGIN (HTTP/2 and HTTP/3, requires TLS) and ALTSVC
    // (HTTP/2) advertisements.
    void advertise_origins(std::span<const std::string_view> origins);
    void advertise_alternative_service(std::string_view value);

    ~context();

    context(const context&) = delete;
    context& operator=(const context&) = delete;
    context(context&&) = delete;
    context& operator=(context&&) = delete;

    [[nodiscard]] context_request req() const& noexcept RUVIA_LIFETIMEBOUND {
        return context_request(*this);
    }
    context_request req() const&& = delete;

    // Connection metadata borrows this request's transport state.
    [[nodiscard]] const conn_info& conn() const& noexcept RUVIA_LIFETIMEBOUND;
    const conn_info& conn() const&& = delete;
    // Copies shared ownership of the state returned by on_connection_open.
    [[nodiscard]] std::shared_ptr<void> connection_state() const noexcept;
    // Closes the entire transport immediately, including every HTTP/2 or
    // HTTP/3 stream. A virtual test connection has no transport to close.
    void abort_connection() noexcept;

    // The exception that failed the current middleware/handler dispatch, or
    // null. Distinct from error(status, code, message) which constructs an
    // error response.
    [[nodiscard]] std::exception_ptr exception() const noexcept {
        return error_;
    }

    // Borrowed for this request. Copy the returned handle when it must outlive
    // the handler; the copy owns a terminal-safe dispatcher endpoint.
    [[nodiscard]] const worker_ref& worker() const& noexcept RUVIA_LIFETIMEBOUND {
        return capabilities_.worker();
    }
    const worker_ref& worker() const&& = delete;

    // Borrows this worker's registered instance; never hand it to another
    // worker. An unregistered type throws std::logic_error.
    template <typename state_type>
    [[nodiscard]] state_type& worker_state() const {
        return capabilities_.worker_state<state_type>();
    }

    // The callable executes on the blocking pool and must own everything it
    // touches. Completion resumes on this worker; options.timeout_ or shutdown
    // stops the wait without joining a still-running foreign callable, and
    // options.stop_token_ is combined with stop_token().
    // Rejection and callable exceptions are retained in blocking_result. Inspect
    // status() for load shedding or use std::move(result).take_value() to throw.
    // With application::blocking_pool(nullptr) the result reports
    // blocking_status::unavailable without submitting the callable.
    template <typename callable_type>
    [[nodiscard]] task<blocking_result<std::invoke_result_t<callable_type&>>> run_blocking(
        callable_type callable, operation_options options = {}) const {
        return capabilities_.run_blocking(std::move(callable), std::move(options));
    }

    // Observes worker shutdown (application::stop() closes every connection
    // at once, with no drain phase) and, for a subrequest, explicit
    // cancellation. db(), redis(), and run_blocking() bind this token
    // automatically. Explicit operation tokens are combined with it.
    //
    // Cancellation is cooperative: framework waits, including streaming sleep(),
    // observe this token; CPU work and raw Asio waits without it are not preempted.
    // Connection phase timeouts close the socket independently.
    //
    // 流式响应（stream()/stream_sse()）的 token 还观察客户端离开：HTTP/1 请求体
    // 完整消费后对端 FIN/RST 或写失败，HTTP/2 本流 RST_STREAM 或连接终止。
    // 缓冲响应路由不监测读侧终止。
    //
    [[nodiscard]] ::ruvia::stop_token stop_token() const noexcept {
        return capabilities_.stop_token();
    }

    // Present only while session_middleware is bound for this request.
    [[nodiscard]] ruvia::session session();

    // Request-scoped typed state: how a middleware hands a value it computed --
    // an authenticated user, a resolved tenant, a trace span -- to everything it
    // calls through next(). The counterpart of worker_state<T>(), which lives for
    // the worker's whole life and is shared by every request on it; this lives
    // for one dynamic next() scope and is private to that request.
    //
    //   task<void> invoke(context& c, next& next_value) {
    //       const auto binding = c.bind_request_state(user);
    //       co_await next_value();                      // handler sees it
    //   }                                               // unbound here
    //
    // The binding stores the value by address and never copies it, so the bound
    // object must outlive the returned handle -- keep both in the middleware's
    // coroutine frame, as above. Binding a temporary is rejected at compile
    // time. One type is one slot: binding T again inside a nested scope shadows
    // the outer binding until the inner handle dies.
    //
    // Deliberately disjoint from req().validated<T>(): that answers "a validator
    // produced and checked this", and hand-bound state must never be able to
    // impersonate it.
    template <typename t_type>
    [[nodiscard]] request_state_binding<t_type> bind_request_state(const t_type& value);

    template <typename t_type>
        requires(!std::is_lvalue_reference_v<t_type>)
    [[nodiscard]] request_state_binding<std::remove_cvref_t<t_type>> bind_request_state(t_type&&) = delete;

    // Throws std::logic_error when nothing of this type is bound.
    template <typename t_type>
    [[nodiscard]] const std::remove_cvref_t<t_type>& request_state() const;

    template <typename t_type>
    [[nodiscard]] const std::remove_cvref_t<t_type>* try_request_state() const noexcept {
        return request_bindings().try_get_state<t_type>();
    }
    [[nodiscard]] const ruvia::env& env() const noexcept;

    // Re-enter the immutable route table on this worker. Authentication,
    // validation and middleware run in a fresh request context.
    [[nodiscard]] task<dispatch_response> dispatch(dispatch_options options);
    [[nodiscard]] bool is_subrequest() const noexcept;

    // Builds a request path from a registered route pattern; the pattern is
    // the route's identity: c.url_for("/users/:id", {"42"}) -> "/users/42".
    // ":name" values are percent-encoded path segments and must be non-empty;
    // the value for a trailing "*" keeps its slashes and may be empty. The
    // result never starts with "//" and never carries a "." or ".." segment.
    // Throws std::invalid_argument for an unregistered pattern, a value-count
    // mismatch, a "."/".." segment, a pattern starting with an empty segment,
    // or a "*" value the route cannot capture back (one holding '\', or
    // starting with '/' at the root), and std::logic_error when the context
    // carries no route table (for example a hand-built test context).
    [[nodiscard]] std::string url_for(
        std::string_view pattern, std::initializer_list<std::string_view> values = {}) const;

    [[nodiscard]] std::string url_for(
        std::string_view pattern, std::span<const std::string_view> values) const;
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
    [[nodiscard]] bool early_data() const noexcept;
    [[nodiscard]] ::ruvia::websocket& websocket() const;
    [[nodiscard]] http_tunnel& tunnel() const;

    [[nodiscard]] response_stream_writer& stream();

    // Sends text/plain; charset=UTF-8 and X-Content-Type-Options: nosniff,
    // like text(); an X-Content-Type-Options the application set is kept.
    [[nodiscard]] response_stream_writer& stream_text();

    [[nodiscard]] sse_writer stream_sse();

    // draft() 只编辑下一次构造的响应草稿，与是否调用 next()/respond() 无关。
    // body/text/json/html/file/static_file/redirect 成功后消费草稿，失败则保留；
    // stream commit 消费草稿。error() 独立构造，不继承草稿或已选响应。
    [[nodiscard]] context_response_editor draft() noexcept {
        return context_response_editor(*this, context_response_editor::target_type::draft);
    }

    // 明确后处理已 respond 的缓冲响应；尚无响应或已提交时修改抛 logic_error。
    // 字段只属于当前响应，下一次替换不继承；跨替换字段显式写入 policy()。
    [[nodiscard]] context_response_editor edit_response() noexcept {
        return context_response_editor(*this, context_response_editor::target_type::selected);
    }

    // 显式跨替换策略，只在最终交付/stream commit 时应用一次；builder 不继承它。
    // header/cookie 覆盖同名或同 cookie 身份的响应字段；append 保留重复语义。
    // 警告：显式 policy().status() 也覆盖错误响应；默认不强制任何状态。
    // response() 观察的是尚未应用 policy 的选定响应，不是最终 wire head。
    // 与 draft/edit_response 一样，提交后任何修改均抛 logic_error。
    [[nodiscard]] context_response_editor policy() noexcept {
        return context_response_editor(*this, context_response_editor::target_type::policy);
    }

    // 只读观察已选择的缓冲响应（不包含尚待应用的 policy）；没有响应则为 nullptr。
    [[nodiscard]] const http_response* response() const noexcept;

    [[nodiscard]] bool committed() const noexcept;
    // 将独立响应移入 context；替换丢弃旧响应全部字段，不自动合并草稿。
    // next() 在 respond() 后启动是控制流错误；已提交后不得替换。
    void respond(http_response&& response);

    // Body builders copy their input into response-owned storage, except
    // static_text, which explicitly borrows static bytes without copying, and
    // std::string&&, which transfers existing string allocations.
    [[nodiscard]] http_response body(std::string_view body = {});
    template <std::same_as<std::string> string_type>
    [[nodiscard]] http_response body(string_type&& body_value) {
        return body_owned(std::move(body_value));
    }
    [[nodiscard]] http_response body(std::span<const std::byte> body);
    [[nodiscard]] http_response body(static_text body);

    // text/plain; charset=UTF-8 with X-Content-Type-Options: nosniff (an
    // application-set X-Content-Type-Options is kept), like stream_text().
    [[nodiscard]] http_response text(std::string_view body);
    template <std::same_as<std::string> string_type>
    [[nodiscard]] http_response text(string_type&& body_value) {
        return text_owned(std::move(body_value));
    }
    [[nodiscard]] http_response text(static_text body);

    template <typename t_type>
        requires detail::is_model_json_value<t_type>
    [[nodiscard]] http_response json(const t_type& value);

    [[nodiscard]] http_response html(std::string_view body);
    template <std::same_as<std::string> string_type>
    [[nodiscard]] http_response html(string_type&& body_value) {
        return html_owned(std::move(body_value));
    }
    [[nodiscard]] http_response html(static_text body);
    [[nodiscard]] http_response redirect(redirect_response_options options);

    // File helpers honor If-Range only for matching strong ETags. Last-Modified
    // dates are weak and do not authorize a partial response.
    //
    // file() and static_file() open the file on this worker's blocking pool,
    // never on the worker, and complete with the settled response: status,
    // validators, Content-Length or Content-Range, and the opened handle the
    // response owns. The pool wait is bounded by server_config::write_timeout_
    // and stop_token(); a wait that cannot complete (no blocking pool, a full
    // queue, the timeout) throws a 503 file_read_unavailable http_error, and
    // a missing pool or a timed-out open is reported to the connection
    // failure channel.
    //
    // Both own their options before they return: the path, relative path and
    // content type are copied (and a static path resolved, its representation
    // selected) by the call, so a stored task never reads the caller's
    // storage. A path that cannot name a file (404), a rejected relative path
    // (403), an unindexed name (404) or an unacceptable coding (406) throws at
    // the call. Only static_file()'s `root` stays borrowed: it must outlive
    // the returned task.
    //
    // file() opens the file once and the response owns that handle; plain-TCP
    // transfers send from it without resolving the path again. The handle is
    // checked again right before sending: a file changed in place since file()
    // fails a single plain-TCP body before its head leaves and aborts any
    // other transfer before its bytes.
    [[nodiscard]] task<http_response> file(file_response_options options);

    // relative_path_ is a decoded file path below the root, such as
    // req().param("*") of a wildcard route; it is never percent-decoded again.
    // '/' and '\\' separate segments. A "." or ".." segment, an empty interior
    // segment, an absolute path, or a NUL is 403 -- rejected, never resolved,
    // whichever parameter it came from -- and an unindexed name 404. A file
    // that is no longer the indexed one is 500 static_file_changed.
    [[nodiscard]] task<http_response> static_file(
        const static_root& root, static_file_response_options options);
    // A temporary root would be destroyed before the returned task reads it.
    task<http_response> static_file(const static_root&&, static_file_response_options) = delete;

    [[nodiscard]] http_response error(http_error_info_options options) const;
    [[nodiscard]] http_response error(const http_error_info& info) const;

    [[nodiscard]] task<http_response> not_found();

private:
    [[nodiscard]] http_response streaming_head();
    [[nodiscard]] http_response body_owned(std::string&& body);
    [[nodiscard]] http_response text_owned(std::string&& body);
    [[nodiscard]] http_response html_owned(std::string&& body);

    [[nodiscard]] task<http_response> not_found_task();
    [[nodiscard]] task<dispatch_response> dispatch_task(std::string method,
        std::string target, std::string body, std::vector<http_header> headers,
        ::ruvia::stop_token stop);
    context& set_stable_response_header(std::string_view name, std::string_view value);
    void apply_response_state(http_response& response, std::optional<http_status_code> status_code);

    [[nodiscard]] http_response json_serialized(std::string& body);

    [[nodiscard]] std::string& decoded_body() const;
    [[nodiscard]] detail::context_request_storage& request_storage() const;
    [[nodiscard]] detail::context_services& services() noexcept;
    [[nodiscard]] const detail::context_services& services() const noexcept;
    void store_response(http_response&& response);
    void store_error(std::exception_ptr exception) noexcept {
        error_ = std::move(exception);
    }
    [[nodiscard]] bool has_response() const noexcept;

    const http_request& request_;
    // context cannot escape request dispatch and therefore borrows the stable
    // server-owned handle without touching its shared ownership count.
    detail::worker_context_capabilities capabilities_;
    std::string_view route_path_;
    const std::string_view* param_names_{nullptr};
    const std::string_view* param_values_{nullptr};
    std::size_t param_count_{0};
    std::uintptr_t route_rate_limit_scope_{0};
    using request_storage_owner_type = std::unique_ptr<detail::context_request_storage>;
    // One typed allocation owns request caches, response/session state,
    // and bindings. Destroyed after operation_scope_ so outstanding operations
    // still see request-owned objects.
    mutable request_storage_owner_type request_storage_;
    std::exception_ptr error_;
    mutable bool body_decoded_ : 1 {false};
    // Content-Encoding was parsed and is empty: request_body() returns the raw
    // bytes without re-reading the header.
    mutable bool body_identity_ : 1 {false};

    [[nodiscard]] const detail::context_request_body_source& request_body_source() const noexcept;
    [[nodiscard]] detail::context_response_output& response_output() noexcept;
    [[nodiscard]] const detail::context_response_output& response_output() const noexcept;
    [[nodiscard]] detail::context_response_state& response_state() noexcept;
    [[nodiscard]] const detail::context_response_state& response_state() const noexcept;
    [[nodiscard]] detail::context_session_state& session_state() noexcept;
    [[nodiscard]] const detail::context_session_state& session_state() const noexcept;
    [[nodiscard]] detail::request_bindings& request_bindings() noexcept;
    [[nodiscard]] const detail::request_bindings& request_bindings() const noexcept;

    // Declared last so it closes first, while every request-owned object and its
    // memory resource are still alive.
    mutable ::ruvia::operation_scope operation_scope_;
};

enum class readiness_state : std::uint8_t {
    ready,
    unavailable,
};

struct readiness_response_options final {
    readiness_state state_{readiness_state::ready};
    borrowed_text unavailable_reason_{"service is not ready"};
};

[[nodiscard]] http_response make_health_response(context& context_value);

[[nodiscard]] http_response make_readiness_response(
    context& context_value, readiness_response_options options = {});

template <typename t_type>
inline request_state_binding<t_type> context::bind_request_state(const t_type& value) {
    return request_bindings().bind_state(value);
}

template <typename t_type>
inline const std::remove_cvref_t<t_type>& context::request_state() const {
    return request_bindings().get_state<t_type>();
}

template <typename t_type>
    requires detail::is_model_json_value<t_type>
inline http_response context::json(const t_type& value) {
    auto body = to_json(value);
    return json_serialized(body);
}

template <typename t_type>
inline std::optional<t_type> context_request::parse_typed(
    std::optional<std::string_view> raw, void (*fail)()) {
    if (!raw) {
        return std::nullopt;
    }
    if constexpr (std::same_as<t_type, bool>) {
        const auto parsed_value = detail::parse_form_bool(*raw);
        if (!parsed_value) {
            fail();
        }
        return *parsed_value;
    } else {
        const auto parsed_value = parse_integer<t_type>(*raw);
        if (parsed_value.index() != 0) {
            fail();
        }
        return std::get<0>(parsed_value);
    }
}

template <detail::parsed_integer t_type>
inline std::optional<t_type> context_request::query(std::string_view name) const {
    return parse_typed<t_type>(query(name), detail::throw_invalid_query);
}

template <std::same_as<bool> t_type>
inline std::optional<bool> context_request::query(std::string_view name) const {
    return parse_typed<t_type>(query(name), detail::throw_invalid_query);
}

template <detail::parsed_integer t_type>
inline std::optional<t_type> context_request::param(std::string_view name) const {
    return parse_typed<t_type>(param(name), detail::throw_invalid_param);
}

template <std::same_as<bool> t_type>
inline std::optional<bool> context_request::param(std::string_view name) const {
    return parse_typed<t_type>(param(name), detail::throw_invalid_param);
}

namespace detail {

template <validation_target target, typename model_type>
[[nodiscard]] task<std::optional<model_type>> parse_optional_request_model(
    const context& context_value, operation_options operation) {
    if (!request_has_body_media_type<target>(context_value.req().header("Content-Type"))) {
        co_return std::nullopt;
    }
    const auto request_body = co_await context_value.req().text(std::move(operation));
    co_return parse_and_validate_request_model<target, model_type>(request_body);
}

}  // namespace detail

template <typename t_type>
task<std::optional<t_type>> context_request::json_if(operation_options operation) const {
    static_assert(detail::is_model<t_type>, "JSON body type must use RUVIA_MODEL");
    return ::ruvia::make_scoped_operation(context_->operation_scope_,
        detail::parse_optional_request_model<validation_target::json, t_type>(*context_, std::move(operation)));
}

template <typename t_type>
task<std::optional<t_type>> context_request::form_if(operation_options operation) const {
    static_assert(detail::is_model<t_type>, "form body type must use RUVIA_MODEL");
    return ::ruvia::make_scoped_operation(context_->operation_scope_,
        detail::parse_optional_request_model<validation_target::form, t_type>(*context_, std::move(operation)));
}

template <typename t_type>
inline const t_type& context_request::validated() const {
    return context_->request_bindings().get_validated<t_type>();
}

}  // namespace ruvia

namespace ruvia::detail {

template <typename t_type>
inline request_binding_guard<t_type> bind_validated_model(context& context_value, const t_type& model) {
    return context_value.request_bindings().bind_validated(model);
}

template <validation_target target>
[[nodiscard]] const request_name_value_list& validation_target_fields(context& c) {
    if constexpr (target == validation_target::query) {
        return c.req().query_fields();
    } else if constexpr (target == validation_target::param) {
        return c.req().param_fields();
    } else if constexpr (target == validation_target::header) {
        return c.req().header_fields();
    } else if constexpr (target == validation_target::cookie) {
        return c.req().cookie_fields();
    } else {
        static_assert(always_false<std::integral_constant<validation_target, target>>,
            "unsupported validator target");
    }
}

template <validation_target target, typename body_type>
task<void> model_validation_middleware<target, body_type>::invoke(context& c, next& next_value) {
    if constexpr (target == validation_target::json || target == validation_target::form) {
        if (!request_has_body_media_type<target>(c.req().header("Content-Type"))) {
            throw_invalid_content_type<target>();
        }
        const auto request_body = co_await c.req().text();
        auto body = parse_and_validate_request_model<target, body_type>(request_body);
        auto binding = bind_validated_model(c, body);
        co_await next_value();
    } else {
        auto body = parse_and_validate_request_model<target, body_type>(
            std::cref(validation_target_fields<target>(c)));
        auto binding = bind_validated_model(c, body);
        co_await next_value();
    }
}

}  // namespace ruvia::detail

namespace ruvia {

class request_stream;
class http_client;
#ifdef RUVIA_ENABLE_DATABASE
class db_client;
#endif
#ifdef RUVIA_ENABLE_REDIS
class redis_client;
#endif
namespace detail {
class router_state;
}

using router_handler = move_only_function<task<http_response>(context&)>;
using router_stream_handler = move_only_function<task<void>(context&)>;

// An explicitly owned, worker-local route engine, independent of application
// and controller discovery. Configure and freeze on its owner worker, then
// await dispatch(stream) from a request_stream callback on that same worker.
// Handlers and middleware instances belong to this router; concurrent requests
// interleave only on its worker. Use one router per worker, not shared mutable
// handlers across workers. Captured references must outlive all dispatches.
// The worker is retained. Configuration, freezing and destruction of the last
// owner run on that worker. A dispatch task retains the frozen state, so the
// facade need not outlive that task; the request still must.
class router final {
public:
    explicit router(worker_ref worker);
    ~router();
    router(const router&) = delete;
    router& operator=(const router&) = delete;
    router(router&&) = delete;
    router& operator=(router&&) = delete;

    // Methods are case-sensitive HTTP tokens. Extension methods have the same
    // static-path restriction as controller routes. Paths and callbacks are
    // owned before return; modifiers reuse controller middleware/body policies.
    template <route_modifier... modifiers>
    router& add(std::string_view method, std::string_view path, router_handler handler) & {
        const std::array<detail::controller_middleware_descriptor, sizeof...(modifiers)> descriptors{detail::make_route_modifier_descriptor<modifiers>()...};
        return add_buffered(method, path, std::move(handler), descriptors);
    }

    template <route_modifier... modifiers>
    router& stream(std::string_view method, std::string_view path, router_stream_handler handler) & {
        const std::array<detail::controller_middleware_descriptor, sizeof...(modifiers)> descriptors{detail::make_route_modifier_descriptor<modifiers>()...};
        return add_stream(method, path, std::move(handler), descriptors);
    }

    // The WebSocket route of `path`: an HTTP/1.1 Upgrade GET or an extended
    // CONNECT (RFC 8441, RFC 9220). The terminal after the route's middleware
    // negotiates through request_stream::accept_websocket(); the handler then
    // runs on context::websocket(). A rejected handshake answers 400 without
    // the handler, as does a request to the path offering no upgrade.
    template <route_modifier... modifiers>
    router& websocket(std::string_view path, router_stream_handler handler, websocket_route_config config = {}) & {
        const std::array<detail::controller_middleware_descriptor, sizeof...(modifiers)> descriptors{detail::make_route_modifier_descriptor<modifiers>()...};
        return add_websocket(path, std::move(handler), std::move(config), descriptors);
    }

    // CONNECT to an exact host:port authority, or "*" for any (RFC 9110
    // §9.3.6). The terminal establishes the tunnel with 200 through
    // request_stream::accept_tunnel(); the handler then runs on
    // context::tunnel().
    template <route_modifier... modifiers>
    router& connect(std::string_view authority, router_stream_handler handler, http_tunnel_route_config config = {}) & {
        const std::array<detail::controller_middleware_descriptor, sizeof...(modifiers)> descriptors{detail::make_route_modifier_descriptor<modifiers>()...};
        return add_tunnel({}, authority, std::move(handler), config, descriptors);
    }

    // An extended CONNECT protocol on `path`; "connect-udp" (RFC 9298) also
    // accepts its HTTP/1.1 Upgrade (with 101) and implies HTTP Datagrams.
    template <route_modifier... modifiers>
    router& connect_protocol(std::string_view protocol, std::string_view path, router_stream_handler handler,
        http_tunnel_route_config config = {}) & {
        const std::array<detail::controller_middleware_descriptor, sizeof...(modifiers)> descriptors{detail::make_route_modifier_descriptor<modifiers>()...};
        return add_tunnel(protocol, path, std::move(handler), config, descriptors);
    }

    template <middleware middleware_type, typename... args_types>
    router& use(args_types&&... args) & {
        return add_middleware(detail::make_owned_middleware_descriptor<middleware_type>(std::forward<args_types>(args)...));
    }

    template <middleware middleware_type, typename... args_types>
    router& use_at(middleware_scope_options options, args_types&&... args) & {
        static_assert(!detail::middleware_runs_on_unmatched_requests<middleware_type>(),
            "middleware serving unmatched requests must be registered with use<T>()");
        return add_scoped_middleware(options.prefix_.view(),
            detail::make_owned_middleware_descriptor<middleware_type>(std::forward<args_types>(args)...));
    }

    // The rule every request this router dispatches passes, matched or not,
    // keyed on the client address like application::rate_limit(). Route
    // ruvia::rate_limit<> rules stack on it. Counted per router, which is
    // per worker; replaces an earlier rule.
    router& rate_limit(rate_limit_config config) &;

    // Lend caller-owned clients and per-worker state to the requests this
    // router dispatches, so context::redis(alias), db(alias), client(alias)
    // and worker_state<T>() -- and middleware built on them, such as
    // session_middleware -- resolve here. A router with any bound client
    // resolves every client lookup only among its own, and one with any bound
    // state every state lookup; otherwise the host's registrations apply.
    // Each client must run on this router's worker (std::invalid_argument
    // otherwise) and, like each state, outlive every dispatch: shut it down
    // only after the host serving this router has shut down. Aliases are
    // unique per kind; "default" is what alias-less lookups name.
#ifdef RUVIA_ENABLE_REDIS
    router& bind_redis(std::string_view alias, const redis_client& client) &;
    router& bind_redis(std::string_view, const redis_client&&) & = delete;
#endif
#ifdef RUVIA_ENABLE_DATABASE
    router& bind_database(std::string_view alias, const db_client& client) &;
    router& bind_database(std::string_view, const db_client&&) & = delete;
#endif
    // alias names this client's default configuration in this router.
    router& bind_http_client(std::string_view alias, const http_client& client) &;
    router& bind_http_client(std::string_view, const http_client&&) & = delete;
    // One instance per type; the router never destroys it.
    template <typename state_type>
    router& bind_worker_state(state_type& state) & {
        return add_worker_state(detail::worker_state_definition::borrow(state));
    }

    router& on_error(http_error_handler handler) &;
    router& on_not_found(http_not_found_handler handler) &;
    router& on_error(scoped_error_handler_options options) &;
    router& on_not_found(scoped_not_found_handler_options options) &;

    // Compiles the shared route/method/parameter/middleware engine and creates
    // this worker's middleware once. Failure leaves configuration editable;
    // success seals every registration. Repeated freeze is a logic error.
    void freeze() &;
    void freeze() && = delete;

    // Single-shot for each request, before another response has been selected.
    // Uses the transport's cancellation, body budgets, response writer,
    // WebSocket/tunnel acceptance and connection services. HEAD/OPTIONS/405/
    // 501, errors, middleware, URL generation and buffered subrequests use the
    // normal route engine.
    // Cold operations retain router state and belong to the request's scope;
    // neither a saved operation nor a handler may outlive its request.
    [[nodiscard]] task<void> dispatch(request_stream& stream) &;
    task<void> dispatch(request_stream&) && = delete;

private:
    router& add_buffered(std::string_view method, std::string_view path, router_handler handler,
        std::span<const detail::controller_middleware_descriptor> modifiers);
    router& add_stream(std::string_view method, std::string_view path, router_stream_handler handler,
        std::span<const detail::controller_middleware_descriptor> modifiers);
    router& add_websocket(std::string_view path, router_stream_handler handler, websocket_route_config config,
        std::span<const detail::controller_middleware_descriptor> modifiers);
    router& add_tunnel(std::string_view protocol, std::string_view target, router_stream_handler handler,
        http_tunnel_route_config config, std::span<const detail::controller_middleware_descriptor> modifiers);
    router& add_worker_state(detail::worker_state_definition definition);
    router& add_middleware(detail::owned_middleware_descriptor descriptor);
    router& add_scoped_middleware(std::string_view prefix, detail::owned_middleware_descriptor descriptor);

    std::shared_ptr<detail::router_state> state_;
};

}  // namespace ruvia

namespace ruvia::detail {

// One canonical spelling is shared by application, test_app and route_table.
// router_impl receives the already-normalized registrations from the public
// configuration facades.
// A trailing slash does not create a different path scope; accepting both
// spellings at one layer and not another makes duplicate fallback handlers
// order-dependent and lets tests exercise a different route graph.
// "/" (or any all-slash spelling) is rejected: it would scope every path and
// silently shadow or duplicate the unscoped registration, which is the only
// app-wide spelling.
[[nodiscard]] inline std::string_view normalize_fallback_prefix(std::string_view prefix) {
    if (prefix.empty() || prefix.front() != '/') {
        throw std::invalid_argument("fallback prefix must start with '/'");
    }
    if ((prefix.find('?') != std::string_view::npos) || !is_valid_http_origin_form_target(prefix)) {
        throw std::invalid_argument("fallback prefix must be an origin-form path without query");
    }
    while (prefix.size() > 1 && prefix.back() == '/') {
        prefix.remove_suffix(1);
    }
    if (prefix == "/") {
        throw std::invalid_argument("fallback prefix must not be '/'; register app-wide without a prefix");
    }
    return prefix;
}

// The one prefix-scoping rule, shared by fallback handler selection and by
// path-scoped middleware. `prefix` is normalized (see normalize_fallback_prefix)
// or empty for an unscoped registration, which scopes everything.
// A prefix matches on whole path segments only, so "/api" scopes "/api" and
// "/api/x" but never "/apix". Segments compare in the
// canonical form route matching and route parameters use (uri_paths_equivalent):
// split on raw '/' and percent-decoded, so "/%61pi/x" is under "/api" exactly
// when it routes like "/api/x", while an encoded "%2F" never acts as a segment
// boundary.
[[nodiscard]] inline bool path_is_under_prefix(
    std::string_view path, std::string_view prefix) noexcept {
    if (prefix.empty()) {
        return true;
    }
    if (path.empty() || path.front() != '/') {
        return false;
    }
    std::size_t path_cursor = 1;
    std::size_t prefix_cursor = 1;
    for (;;) {
        const auto prefix_slash = prefix.find('/', prefix_cursor);
        const auto prefix_end = prefix_slash == std::string_view::npos ? prefix.size() : prefix_slash;
        const auto path_slash = path.find('/', path_cursor);
        const auto path_end = path_slash == std::string_view::npos ? path.size() : path_slash;
        if (!uri_paths_equivalent(path.substr(path_cursor, path_end - path_cursor),
                prefix.substr(prefix_cursor, prefix_end - prefix_cursor))) {
            return false;
        }
        if (prefix_end == prefix.size()) {
            // path_end is a '/' or the end of the path: a segment boundary.
            return true;
        }
        if (path_end == path.size()) {
            return false;
        }
        prefix_cursor = prefix_end + 1;
        path_cursor = path_end + 1;
    }
}

// Two normalized prefixes that scope the same request paths, such as "/api"
// and "/%61pi", are one registration.
[[nodiscard]] inline bool fallback_prefixes_equivalent(std::string_view left, std::string_view right) noexcept {
    return path_is_under_prefix(left, right) && path_is_under_prefix(right, left);
}

// Segment count of a normalized prefix. A scope nested inside another always
// has more segments, whatever either one's escaping.
[[nodiscard]] inline std::size_t fallback_prefix_depth(std::string_view prefix) noexcept {
    std::size_t depth = 0;
    for (const auto c : prefix) {
        depth += c == '/' ? 1U : 0U;
    }
    return depth;
}

class router;

template <typename callback_type>
using callback_registration = std::unique_ptr<callback_type>;

// Pin configured callables before entering the application lock: publication
// only swaps owning pointers, and router views borrow an address-stable owner.
template <typename signature_type>
[[nodiscard]] callback_registration<move_only_function<signature_type>> own_callback(
    move_only_function<signature_type>&& callback) {
    if (!callback) {
        return {};
    }
    return std::make_unique<move_only_function<signature_type>>(std::move(callback));
}

template <middleware middleware_type, typename... args_types>
[[nodiscard]] owned_middleware_descriptor make_scoped_app_middleware(
    const middleware_scope_options& options, args_types&&... args) {
    static_assert(!middleware_runs_on_unmatched_requests<middleware_type>(),
        "a middleware declaring ruvia_runs_on_unmatched_requests cannot be path-scoped with "
        "use_at(); register it app-wide with use<T>()");
    const auto normalized = normalize_fallback_prefix(options.prefix_.view());
    return make_owned_middleware_descriptor<middleware_type>(std::forward<args_types>(args)...)
        .scoped_to(normalized);
}

template <typename state_type>
[[nodiscard]] worker_state_definition make_default_worker_state() {
    static_assert(std::is_default_constructible_v<state_type>,
        "use_worker_state<T>() without a factory requires T to be default "
        "constructible; pass a factory otherwise");
    return worker_state_definition::make<state_type>([] { return state_type(); });
}

// application and test_app own this same dispatch configuration. Their lifecycle guards
// remain outside the component; validation, callback ownership, registration
// storage and router binding have one implementation. Bound callback views
// borrow this immovable owner until its routers have retired.
class app_configuration final {
public:
    app_configuration();

    app_configuration(const app_configuration&) = delete;
    app_configuration& operator=(const app_configuration&) = delete;
    app_configuration(app_configuration&&) = delete;
    app_configuration& operator=(app_configuration&&) = delete;

    void add_middleware(owned_middleware_descriptor&& descriptor);
    void add_worker_state(worker_state_definition&& definition);
    void on_error(callback_registration<http_error_handler>&& handler);
    void on_error(borrowed_text prefix, callback_registration<http_error_handler>&& handler);
    // Returns the cleared handler so the caller destroys it after releasing its lock.
    [[nodiscard]] callback_registration<http_error_handler> on_error(std::nullptr_t) noexcept;
    void on_not_found(callback_registration<http_not_found_handler>&& handler);
    void on_not_found(borrowed_text prefix, callback_registration<http_not_found_handler>&& handler);
    [[nodiscard]] callback_registration<http_not_found_handler> on_not_found(std::nullptr_t) noexcept;

    [[nodiscard]] std::span<const worker_state_definition> worker_states() const noexcept {
        return worker_states_;
    }

    void apply(router& routes) const;

private:
    callback_registration<http_error_handler> error_handler_;
    callback_registration<http_not_found_handler> not_found_handler_;
    std::vector<std::pair<std::string, callback_registration<http_error_handler>>> prefix_error_handlers_;
    std::vector<std::pair<std::string, callback_registration<http_not_found_handler>>> prefix_not_found_handlers_;
    std::vector<owned_middleware_descriptor> middlewares_;
    std::vector<worker_state_definition> worker_states_;
};

// Internal startup-time controller registration contracts.

template <typename controller_type>
class controller_registration_access;

class controller_store;

// One registered controller type: how a worker constructs its instance, and
// how that instance declares its routes. Routes are declared once, into the
// process route plan; every worker afterwards only constructs instances.
struct controller_registrar final {
    void* (*create_)(controller_store&){nullptr};
    void (*register_routes_)(void* controller, router&){nullptr};

    [[nodiscard]] friend bool operator==(
        const controller_registrar&, const controller_registrar&) noexcept = default;
};

// The controller instances of one worker, one per registrar in registrar
// order, which is the order the route plan's controller slots use.
class controller_store final {
public:
    controller_store();
    explicit controller_store(std::span<const controller_registrar> registrars);
    ~controller_store();

    controller_store(const controller_store&) = delete;
    controller_store& operator=(const controller_store&) = delete;
    controller_store(controller_store&&) noexcept;
    controller_store& operator=(controller_store&&) noexcept;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] void* instance(std::size_t slot) const noexcept;

private:
    template <typename controller_type>
    friend void* create_controller(controller_store& controllers);

    template <typename t_type>
    t_type& emplace() {
        auto* raw = new t_type();
        try {
            add_lifetime(raw, &controller_store::destroy<t_type>);
        } catch (...) {
            delete raw;
            throw;
        }
        return *raw;
    }

    using destroy_type = void (*)(void*) noexcept;

    struct controller_lifetime final {
        void* target_{nullptr};
        destroy_type destroy_{nullptr};

        controller_lifetime(void* target_value, destroy_type destroy_value) noexcept
            : target_(target_value),
              destroy_(destroy_value) {}
        controller_lifetime(const controller_lifetime&) = delete;
        controller_lifetime& operator=(const controller_lifetime&) = delete;
        controller_lifetime(controller_lifetime&& other) noexcept
            : target_(std::exchange(other.target_, nullptr)),
              destroy_(std::exchange(other.destroy_, nullptr)) {}
        void reset() noexcept {
            auto* const target = std::exchange(target_, nullptr);
            auto* const destroy = std::exchange(destroy_, nullptr);
            if (target != nullptr && destroy != nullptr) {
                destroy(target);
            }
        }
        controller_lifetime& operator=(controller_lifetime&& other) noexcept {
            if (this != &other) {
                reset();
                target_ = std::exchange(other.target_, nullptr);
                destroy_ = std::exchange(other.destroy_, nullptr);
            }
            return *this;
        }
        ~controller_lifetime() {
            reset();
        }
    };

    struct controller_lifetime_list final {
        std::vector<controller_lifetime> items_{};

        controller_lifetime_list() = default;
        controller_lifetime_list(const controller_lifetime_list&) = delete;
        controller_lifetime_list& operator=(const controller_lifetime_list&) = delete;
        controller_lifetime_list(controller_lifetime_list&&) noexcept = default;
        controller_lifetime_list& operator=(controller_lifetime_list&& other) noexcept {
            if (this != &other) {
                clear_reverse();
                items_ = std::move(other.items_);
            }
            return *this;
        }
        ~controller_lifetime_list() {
            clear_reverse();
        }
        void clear_reverse() noexcept {
            while (!items_.empty()) {
                items_.pop_back();
            }
        }
        void reserve(std::size_t count) {
            items_.reserve(count);
        }
        [[nodiscard]] std::size_t size() const noexcept {
            return items_.size();
        }
        [[nodiscard]] const controller_lifetime& operator[](std::size_t index) const noexcept {
            return items_[index];
        }
        template <typename... args_type>
        void emplace_back(args_type&&... args) {
            items_.emplace_back(std::forward<args_type>(args)...);
        }
    };

    void add_lifetime(void* target, destroy_type destroy);

    template <typename t_type>
    static void destroy(void* target) noexcept {
        delete static_cast<t_type*>(target);
    }

    controller_lifetime_list lifetimes_{};
};

class controller_route_builder final {
public:
    controller_route_builder(const controller_route_builder&) = delete;
    controller_route_builder& operator=(const controller_route_builder&) = delete;
    ~controller_route_builder();

private:
    template <typename controller_type>
    friend class controller_registration_access;

    controller_route_builder(
        router& router_value, std::string_view prefix, std::vector<controller_middleware_descriptor> middlewares);
    // Each builds the endpoint its route kind runs and registers it through
    // the router's single registration path.
    void register_route(http_known_method method, std::string_view method_token, std::string_view path,
        route_handler handler, std::span<const controller_middleware_descriptor> middlewares) const;
    void register_route(http_known_method method, std::string_view method_token, std::string_view path,
        route_stream_handler handler, std::span<const controller_middleware_descriptor> middlewares) const;
    void register_tunnel_route(std::string_view protocol, std::string_view target, route_stream_handler handler,
        std::span<const controller_middleware_descriptor> middlewares, http_tunnel_route_config config) const;
    void register_websocket_route(http_known_method method, std::string_view path,
        route_stream_handler handler, std::span<const controller_middleware_descriptor> middlewares,
        websocket_route_config websocket_config) const;
    [[nodiscard]] controller_route_builder create_scope(
        std::string_view prefix, std::span<const controller_middleware_descriptor> middlewares) const;

    struct owned_prefix_tag_type final {};
    controller_route_builder(router& router_value, std::string prefix,
        std::vector<controller_middleware_descriptor> middlewares, owned_prefix_tag_type);

    router& router_;
    std::string prefix_;
    std::vector<controller_middleware_descriptor> middlewares_;
};

[[nodiscard]] bool add_controller_registrar(controller_registrar registrar_value);
// Static controller discovery is complete before main. The first application/test_app
// build seals this registry; loading a controller-bearing module afterwards is
// a startup contract error instead of silently changing only later workers.
[[nodiscard]] std::vector<controller_registrar> seal_controller_registrars();

template <typename controller_type>
void* create_controller(controller_store& controllers) {
    return &controllers.emplace<controller_type>();
}

template <typename controller_type>
void register_controller_routes(void* controller_value, router& router_value) {
    controller_registration_access<controller_type>::register_routes(
        *static_cast<controller_type*>(controller_value), router_value);
}

template <typename controller_type>
[[nodiscard]] bool register_controller() {
    static_assert(std::is_final_v<controller_type>, "controller must be final");
    static_assert(
        std::is_default_constructible_v<controller_type>, "controller must be default constructible");

    return add_controller_registrar(
        {.create_ = &create_controller<controller_type>, .register_routes_ = &register_controller_routes<controller_type>});
}

template <typename member_type>
struct context_task_result {
    static constexpr bool ok = false;
    using type = void;
};

template <typename controller_type, typename result_type>
struct context_task_result<task<result_type> (controller_type::*)(context&)> {
    static constexpr bool ok = true;
    using type = result_type;
};

template <typename controller_type, typename result_type>
struct context_task_result<task<result_type> (controller_type::*)(context&) const> {
    static constexpr bool ok = true;
    using type = result_type;
};

// Unevaluated probe used by RUVIA_GET_WS / RUVIA_CONNECT / RUVIA_CONNECT_PROTOCOL
// to distinguish a leading route-config expression `(config) + probe` (binary +)
// from a leading middleware type `(M) + probe` (C-style cast of +probe to M).
struct route_arg_probe final {
    [[nodiscard]] constexpr route_arg_probe operator+() const noexcept {
        return {};
    }

    template <typename target_type>
        requires(!std::same_as<std::remove_cvref_t<target_type>, websocket_route_config> &&
                 !std::same_as<std::remove_cvref_t<target_type>, http_tunnel_route_config>)
    [[nodiscard]] constexpr operator target_type() const noexcept;
};

[[nodiscard]] inline websocket_route_config operator+(
    websocket_route_config config, route_arg_probe) noexcept {
    return config;
}

[[nodiscard]] inline constexpr http_tunnel_route_config operator+(
    http_tunnel_route_config config, route_arg_probe) noexcept {
    return config;
}

template <typename controller_type>
class controller_registration_access final {
    friend controller_type;

    template <typename t_type>
    friend void register_controller_routes(void* controller_value, router& router_value);

    using middleware_list_type = std::vector<controller_middleware_descriptor>;

    [[nodiscard]] static constexpr std::string_view group_prefix() noexcept {
        if constexpr (requires { controller_type::ruvia_controller_group_prefix(); }) {
            return controller_type::ruvia_controller_group_prefix();
        } else {
            return {};
        }
    }

    [[nodiscard]] static middleware_list_type group_middlewares() {
        if constexpr (requires { controller_type::ruvia_controller_group_middlewares(); }) {
            return controller_type::ruvia_controller_group_middlewares();
        } else {
            return make_middlewares<>();
        }
    }

    [[nodiscard]] static controller_route_builder create_route_group(
        router& router_value, std::string_view prefix, middleware_list_type middlewares) {
        return controller_route_builder(router_value, prefix, std::move(middlewares));
    }

    [[nodiscard]] static controller_route_builder create_route_group(
        const controller_route_builder& scope, std::string_view prefix,
        const middleware_list_type& middlewares) {
        return scope.create_scope(prefix, middlewares);
    }

    static void add_route(const controller_route_builder& scope, http_known_method method,
        std::string_view method_token, std::string_view path, route_handler handler,
        std::span<const controller_middleware_descriptor> middlewares) {
        scope.register_route(method, method_token, path, handler, middlewares);
    }

    static void add_route(const controller_route_builder& scope, http_known_method method,
        std::string_view method_token, std::string_view path, route_stream_handler handler,
        std::span<const controller_middleware_descriptor> middlewares) {
        scope.register_route(method, method_token, path, handler, middlewares);
    }

    static void add_tunnel_route(const controller_route_builder& scope, std::string_view protocol,
        std::string_view target, route_stream_handler handler,
        std::span<const controller_middleware_descriptor> middlewares, http_tunnel_route_config config = {}) {
        scope.register_tunnel_route(protocol, target, handler, middlewares, config);
    }

    static void add_websocket_route(const controller_route_builder& scope, http_known_method method,
        std::string_view path, route_stream_handler handler,
        std::span<const controller_middleware_descriptor> middlewares,
        websocket_route_config websocket_config = {}) {
        scope.register_websocket_route(
            method, path, handler, middlewares, std::move(websocket_config));
    }

    template <auto handler>
    [[nodiscard]] static auto bind(controller_type* instance) noexcept {
        using result_type = typename context_task_result<decltype(handler)>::type;
        static_assert(context_task_result<decltype(handler)>::ok,
            "handler must take context& and return task<http_response> or task<void>");
        static_assert(std::is_same_v<result_type, http_response> || std::is_same_v<result_type, void>,
            "handlers must return task<http_response> or task<void>; serialize response models with c.json(model)");
        if constexpr (std::is_same_v<result_type, void>) {
            return callback_access::make<task<void>(context&)>(instance, &invoke_stream<handler>);
        } else {
            return callback_access::make<task<http_response>(context&)>(instance, &invoke<handler>);
        }
    }

    template <auto handler>
    [[nodiscard]] static route_stream_handler bind_stream(controller_type* instance) noexcept {
        using result_type = typename context_task_result<decltype(handler)>::type;
        static_assert(context_task_result<decltype(handler)>::ok && std::is_same_v<result_type, void>,
            "stream/upgrade/tunnel handler must take context& and return task<void>");
        return callback_access::make<task<void>(context&)>(instance, &invoke_stream<handler>);
    }

    template <route_modifier... modifier_types>
    [[nodiscard]] static middleware_list_type make_middlewares() {
        middleware_list_type middlewares;
        if constexpr (sizeof...(modifier_types) > 0) {
            middlewares.reserve(sizeof...(modifier_types));
            (middlewares.push_back(make_route_modifier_descriptor<modifier_types>()), ...);
        }
        return middlewares;
    }

    static void register_routes(controller_type& controller_value, router& router_value) {
        controller_value.register_routes(router_value);
    }

    template <auto handler>
    [[nodiscard]] static task<http_response> invoke(void* target, context& context_value) {
        return (static_cast<controller_type*>(target)->*handler)(context_value);
    }

    template <auto handler>
    [[nodiscard]] static task<void> invoke_stream(void* target, context& context_value) {
        return (static_cast<controller_type*>(target)->*handler)(context_value);
    }
};

// Methods covered by RUVIA_ALL. HEAD is intentionally omitted: buffered and
// response-stream (including SSE) GET routes provide an implicit HEAD fallback
// without another route record (RFC 9110 9.3.2: same handler and head fields,
// body suppressed); websocket GET routes require an explicit HEAD route.
inline constexpr std::array ruvia_all_route_methods = {http_known_method::get, http_known_method::post,
    http_known_method::put, http_known_method::patch, http_known_method::delete_value,
    http_known_method::options};

// Startup-only holder for the RUVIA_ON method list; the macro pastes the
// parenthesized list as a constructor call.
template <std::size_t n>
class ruvia_method_list final {
public:
    template <std::same_as<http_known_method>... methods_type>
    constexpr explicit ruvia_method_list(methods_type... methods) noexcept
        : methods_{methods...} {
        static_assert(sizeof...(methods_type) > 0, "RUVIA_ON requires at least one method");
        static_assert(sizeof...(methods_type) == n);
    }

    [[nodiscard]] constexpr const http_known_method* begin() const& noexcept {
        return methods_.data();
    }
    [[nodiscard]] constexpr const http_known_method* begin() const&& = delete;

    [[nodiscard]] constexpr const http_known_method* end() const& noexcept {
        return methods_.data() + n;
    }
    [[nodiscard]] constexpr const http_known_method* end() const&& = delete;

private:
    std::array<http_known_method, n> methods_{};
};

template <typename... methods_type>
ruvia_method_list(methods_type...) -> ruvia_method_list<sizeof...(methods_type)>;

// Startup-only holder for the RUVIA_ON path list; the macro pastes the
// parenthesized list as a constructor call.
template <std::size_t n>
class ruvia_path_list final {
public:
    template <typename... paths_type>
        requires(std::constructible_from<borrowed_text, paths_type&&> && ...)
    constexpr explicit ruvia_path_list(paths_type&&... paths) noexcept
        : paths_{borrowed_text(std::forward<paths_type>(paths)).view()...} {
        static_assert(sizeof...(paths_type) > 0, "RUVIA_ON requires at least one path");
        static_assert(sizeof...(paths_type) == n);
    }

    template <typename... paths_type>
        requires((std::convertible_to<paths_type&&, std::string_view> && ...) &&
                    (!std::constructible_from<borrowed_text, paths_type&&> || ...))
    explicit ruvia_path_list(paths_type&&...) = delete;

    [[nodiscard]] constexpr const std::string_view* begin() const& noexcept {
        return paths_.data();
    }
    [[nodiscard]] constexpr const std::string_view* begin() const&& = delete;

    [[nodiscard]] constexpr const std::string_view* end() const& noexcept {
        return paths_.data() + n;
    }
    [[nodiscard]] constexpr const std::string_view* end() const&& = delete;

private:
    std::array<std::string_view, n> paths_{};
};

template <typename... paths_type>
ruvia_path_list(paths_type&&...) -> ruvia_path_list<sizeof...(paths_type)>;

}  // namespace ruvia::detail

#define RUVIA_CONTROLLER_GROUP(prefix, ...)                                                      \
private:                                                                                         \
    [[nodiscard]] static constexpr ::std::string_view ruvia_controller_group_prefix() noexcept { \
        static_assert(::std::constructible_from<::ruvia::borrowed_text, decltype((prefix))>,     \
            "controller group prefixes must outlive route registration");                        \
        return ::ruvia::borrowed_text(prefix).view();                                            \
    }                                                                                            \
    [[nodiscard]] static auto ruvia_controller_group_middlewares() {                             \
        return ::ruvia::detail::controller_registration_access<                                  \
            ruvia_controller_type>::template make_middlewares<__VA_ARGS__>();                    \
    }

// A controller is a final, default-constructible class that derives from
// nothing. RUVIA_ROUTES_BEGIN names it because the registration flag
// RUVIA_ROUTES_END declares is a static data member initializer, which is not a
// complete-class context and so cannot deduce the enclosing type.
#define RUVIA_ROUTES_BEGIN(controller_type)                                                                   \
private:                                                                                                      \
    using ruvia_controller_type = controller_type;                                                            \
    using ruvia_controller_access_type =                                                                      \
        ::ruvia::detail::controller_registration_access<ruvia_controller_type>;                               \
    friend class ::ruvia::detail::controller_registration_access<ruvia_controller_type>;                      \
    void register_routes(::ruvia::detail::router& router) {                                                   \
        static_assert(::std::is_same_v<ruvia_controller_type, ::std::remove_cvref_t<decltype(*this)>>,        \
            "RUVIA_ROUTES_BEGIN must name its enclosing controller");                                         \
        auto ruvia_controller_group = ruvia_controller_access_type::create_route_group(router,                \
            ruvia_controller_access_type::group_prefix(), ruvia_controller_access_type::group_middlewares()); \
        [[maybe_unused]] auto& ruvia_route_scope = ruvia_controller_group;

#if defined(_MSC_VER)
// selectany is invalid when a controller has internal linkage (for example in
// an anonymous namespace). MSVC already retains dynamic inline-variable
// initialization, so no additional declaration attribute is needed here.
#define RUVIA_DETAIL_CONTROLLER_RETAIN
#elif defined(__GNUC__) || defined(__clang__)
#define RUVIA_DETAIL_CONTROLLER_RETAIN __attribute__((used))
#else
#define RUVIA_DETAIL_CONTROLLER_RETAIN
#endif

#define RUVIA_ROUTES_END                                                                   \
    }                                                                                      \
    RUVIA_DETAIL_CONTROLLER_RETAIN inline static const bool ruvia_controller_registered_ = \
        ::ruvia::detail::register_controller<ruvia_controller_type>();

#define RUVIA_DETAIL_ADD_METHOD_ROUTE(method, path, handler, ...)                           \
    ruvia_controller_access_type::add_route(ruvia_route_scope, method, {}, path,            \
        ruvia_controller_access_type::template bind<&ruvia_controller_type::handler>(this), \
        ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>())

// Route paths and request paths compare in one canonical form
// (ruvia::uri_paths_equivalent): segments split on literal '/', each compared
// by its percent-decoded octets, so "/users/%61dmin" reaches the "/users/admin"
// route and its middleware, hex case never matters, and "%2F" stays inside its
// segment. Path-scoped middleware, scoped fallbacks and decoded route
// parameters use the same form; two patterns differing only in spelling are a
// duplicate registration. A GET route also answers HEAD unless it is a
// websocket route (see ruvia_all_route_methods).
#define RUVIA_GET(path, handler, ...) \
    RUVIA_DETAIL_ADD_METHOD_ROUTE(::ruvia::http_known_method::get, path, handler __VA_OPT__(, ) __VA_ARGS__)

#define RUVIA_DETAIL_SELECT_SECOND(first, second, ...) second
#define RUVIA_DETAIL_CHOOSE_VA_MACRO(with_args, without_args, ...) \
    RUVIA_DETAIL_SELECT_SECOND(__VA_OPT__(, ) with_args, without_args)

#define RUVIA_DETAIL_ADD_TUNNEL_ROUTE_NO_ARGS(protocol, target, handler)                           \
    ruvia_controller_access_type::add_tunnel_route(ruvia_route_scope, protocol, target,            \
        ruvia_controller_access_type::template bind_stream<&ruvia_controller_type::handler>(this), \
        ruvia_controller_access_type::template make_middlewares<>())

#define RUVIA_DETAIL_ADD_TUNNEL_ROUTE_WITH_ARGS(protocol, target, handler, first, ...)                     \
    [&](auto ruvia_route_probe) {                                                                          \
        using ruvia_first_arg_type = ::std::conditional_t<sizeof(ruvia_route_probe) != 0,                  \
            ::std::remove_cvref_t<decltype((first) + ruvia_route_probe)>, void>;                           \
        if constexpr (::std::is_same_v<ruvia_first_arg_type, ::ruvia::http_tunnel_route_config>) {         \
            ruvia_controller_access_type::add_tunnel_route(ruvia_route_scope, protocol, target,            \
                ruvia_controller_access_type::template bind_stream<&ruvia_controller_type::handler>(this), \
                ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>(),                    \
                static_cast<ruvia_first_arg_type>((first) + ruvia_route_probe));                           \
        } else {                                                                                           \
            ruvia_controller_access_type::add_tunnel_route(ruvia_route_scope, protocol, target,            \
                ruvia_controller_access_type::template bind_stream<&ruvia_controller_type::handler>(this), \
                ruvia_controller_access_type::template make_middlewares<                                   \
                    ruvia_first_arg_type __VA_OPT__(, ) __VA_ARGS__>());                                   \
        }                                                                                                  \
    }(::ruvia::detail::route_arg_probe{})

// Ordinary CONNECT uses an exact host:port authority or "*". The only
// upgrade protocol is "connect-udp"; any other fails registration with
// std::invalid_argument. Group prefixes apply only to upgrade-protocol tunnel
// paths; controller/group middleware always runs.
#define RUVIA_CONNECT(authority, handler, ...)                            \
    RUVIA_DETAIL_CHOOSE_VA_MACRO(RUVIA_DETAIL_ADD_TUNNEL_ROUTE_WITH_ARGS, \
        RUVIA_DETAIL_ADD_TUNNEL_ROUTE_NO_ARGS, __VA_ARGS__)({}, authority, handler __VA_OPT__(, ) __VA_ARGS__)

#define RUVIA_CONNECT_PROTOCOL(protocol, path, handler, ...)              \
    RUVIA_DETAIL_CHOOSE_VA_MACRO(RUVIA_DETAIL_ADD_TUNNEL_ROUTE_WITH_ARGS, \
        RUVIA_DETAIL_ADD_TUNNEL_ROUTE_NO_ARGS, __VA_ARGS__)(protocol, path, handler __VA_OPT__(, ) __VA_ARGS__)

#define RUVIA_DETAIL_ADD_WS_ROUTE_NO_ARGS(path, handler)                                                  \
    ruvia_controller_access_type::add_websocket_route(ruvia_route_scope, ::ruvia::http_known_method::get, \
        path, ruvia_controller_access_type::template bind_stream<&ruvia_controller_type::handler>(this),  \
        ruvia_controller_access_type::template make_middlewares<>())

#define RUVIA_DETAIL_ADD_WS_ROUTE_WITH_ARGS(path, handler, first, ...)                                     \
    [&](auto ruvia_route_probe) {                                                                          \
        using ruvia_first_arg_type = ::std::conditional_t<sizeof(ruvia_route_probe) != 0,                  \
            ::std::remove_cvref_t<decltype((first) + ruvia_route_probe)>, void>;                           \
        if constexpr (::std::is_same_v<ruvia_first_arg_type, ::ruvia::websocket_route_config>) {           \
            ruvia_controller_access_type::add_websocket_route(ruvia_route_scope,                           \
                ::ruvia::http_known_method::get, path,                                                     \
                ruvia_controller_access_type::template bind_stream<&ruvia_controller_type::handler>(this), \
                ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>(),                    \
                static_cast<ruvia_first_arg_type>((first) + ruvia_route_probe));                           \
        } else {                                                                                           \
            ruvia_controller_access_type::add_websocket_route(ruvia_route_scope,                           \
                ::ruvia::http_known_method::get, path,                                                     \
                ruvia_controller_access_type::template bind_stream<&ruvia_controller_type::handler>(this), \
                ruvia_controller_access_type::template make_middlewares<                                   \
                    ruvia_first_arg_type __VA_OPT__(, ) __VA_ARGS__>());                                   \
        }                                                                                                  \
    }(::ruvia::detail::route_arg_probe{})

#define RUVIA_GET_WS(path, handler, ...)                              \
    RUVIA_DETAIL_CHOOSE_VA_MACRO(RUVIA_DETAIL_ADD_WS_ROUTE_WITH_ARGS, \
        RUVIA_DETAIL_ADD_WS_ROUTE_NO_ARGS, __VA_ARGS__)(path, handler __VA_OPT__(, ) __VA_ARGS__)

#define RUVIA_POST(path, handler, ...) \
    RUVIA_DETAIL_ADD_METHOD_ROUTE(::ruvia::http_known_method::post, path, handler __VA_OPT__(, ) __VA_ARGS__)

#define RUVIA_PUT(path, handler, ...) \
    RUVIA_DETAIL_ADD_METHOD_ROUTE(::ruvia::http_known_method::put, path, handler __VA_OPT__(, ) __VA_ARGS__)

#define RUVIA_DELETE(path, handler, ...) \
    RUVIA_DETAIL_ADD_METHOD_ROUTE(::ruvia::http_known_method::delete_value, path, handler __VA_OPT__(, ) __VA_ARGS__)

#define RUVIA_PATCH(path, handler, ...) \
    RUVIA_DETAIL_ADD_METHOD_ROUTE(::ruvia::http_known_method::patch, path, handler __VA_OPT__(, ) __VA_ARGS__)

#define RUVIA_HEAD(path, handler, ...) \
    RUVIA_DETAIL_ADD_METHOD_ROUTE(::ruvia::http_known_method::head, path, handler __VA_OPT__(, ) __VA_ARGS__)

#define RUVIA_OPTIONS(path, handler, ...) \
    RUVIA_DETAIL_ADD_METHOD_ROUTE(::ruvia::http_known_method::options, path, handler __VA_OPT__(, ) __VA_ARGS__)

// An extension method, routed by its exact wire token: PROPFIND, PURGE, LOCK.
// HTTP's method space is open (http_known_method is only this framework's fixed
// classification, never the wire value), but routes were keyed on that enum, so
// anything outside it answered 501 and could not be served at all.
//
// The token is case-sensitive per RFC 9110 9.1, must be a valid method token,
// and must NOT be one of the classified methods -- those keep the enum as their
// identity and their own macros. The path must be static; only the classified
// methods have dynamic-segment routes.
#define RUVIA_METHOD(method_token, path, handler, ...)                                              \
    ruvia_controller_access_type::add_route(ruvia_route_scope, ::ruvia::http_known_method::unknown, \
        method_token, path,                                                                         \
        ruvia_controller_access_type::template bind<&ruvia_controller_type::handler>(this),         \
        ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>())

// Hono app.all: registers the handler for GET/POST/PUT/PATCH/DELETE/OPTIONS.
#define RUVIA_ALL(path, handler, ...)                                                               \
    {                                                                                               \
        auto&& ruvia_route_path = (path);                                                           \
        for (const auto ruvia_route_method : ::ruvia::detail::ruvia_all_route_methods) {            \
            ruvia_controller_access_type::add_route(ruvia_route_scope, ruvia_route_method, {},      \
                ruvia_route_path,                                                                   \
                ruvia_controller_access_type::template bind<&ruvia_controller_type::handler>(this), \
                ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>());            \
        }                                                                                           \
    }

// Hono app.on: registers the handler for an explicit method x path list, e.g.
// RUVIA_ON((ruvia::http_known_method::put, ruvia::http_known_method::delete_value),
//     ("/items/:id", "/legacy/:id"), handler).
#define RUVIA_ON(methods, paths, handler, ...)                                                          \
    {                                                                                                   \
        auto ruvia_route_methods = ::ruvia::detail::ruvia_method_list methods;                          \
        auto ruvia_route_paths = ::ruvia::detail::ruvia_path_list paths;                                \
        for (const auto ruvia_route_method : ruvia_route_methods) {                                     \
            for (const auto ruvia_route_path : ruvia_route_paths) {                                     \
                ruvia_controller_access_type::add_route(ruvia_route_scope, ruvia_route_method, {},      \
                    ruvia_route_path,                                                                   \
                    ruvia_controller_access_type::template bind<&ruvia_controller_type::handler>(this), \
                    ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>());            \
            }                                                                                           \
        }                                                                                               \
    }
#define RUVIA_GROUP_BEGIN(prefix, ...)                                                               \
    {                                                                                                \
        static_assert(::std::constructible_from<::ruvia::borrowed_text, decltype((prefix))>,         \
            "route group prefixes must outlive route registration");                                 \
        auto ruvia_route_group = ruvia_controller_access_type::create_route_group(ruvia_route_scope, \
            ::ruvia::borrowed_text(prefix).view(),                                                   \
            ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>());                 \
        auto& ruvia_route_scope = ruvia_route_group;

#define RUVIA_GROUP_END }
