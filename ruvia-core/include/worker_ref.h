#pragma once

#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include "delivery_result.h"
#include "move_only_function.h"
#include "worker_timer.h"

namespace ruvia {

using worker_id_type = std::uint64_t;

// Stable worker-selection hash of `key` (64-bit FNV-1a; identical on every platform).
[[nodiscard]] std::uint64_t worker_selection_hash(std::string_view key) noexcept;

class blocking_pool;

// An owner observes its worker's shutdown through `void worker_stopping()
// noexcept`, run in the first notification pass, and/or `void
// worker_stopping_complete() noexcept`, run in the second pass after every
// registered owner has seen the first.
template <typename owner_type>
concept worker_shutdown_observer = requires(owner_type& owner) {
    { owner.worker_stopping() } noexcept;
} || requires(owner_type& owner) {
    { owner.worker_stopping_complete() } noexcept;
};

namespace detail {
class worker_dispatcher;

// Per-owner-type phase table; a null entry skips that phase.
struct worker_shutdown_operations_type final {
    using phase_type = void (*)(void* owner) noexcept;

    phase_type stopping_;
    phase_type stopping_complete_;
};

template <typename owner_type>
[[nodiscard]] consteval worker_shutdown_operations_type make_worker_shutdown_operations() noexcept {
    worker_shutdown_operations_type operations{nullptr, nullptr};
    if constexpr (requires(owner_type& owner) {
                      { owner.worker_stopping() } noexcept;
                  }) {
        operations.stopping_ = [](void* owner) noexcept {
            static_cast<owner_type*>(owner)->worker_stopping();
        };
    }
    if constexpr (requires(owner_type& owner) {
                      { owner.worker_stopping_complete() } noexcept;
                  }) {
        operations.stopping_complete_ = [](void* owner) noexcept {
            static_cast<owner_type*>(owner)->worker_stopping_complete();
        };
    }
    return operations;
}

template <typename owner_type>
inline constexpr worker_shutdown_operations_type worker_shutdown_operations = make_worker_shutdown_operations<owner_type>();

}  // namespace detail

// Intrusive link of one shared owner into its worker's shutdown registry,
// held as a member of that owner (see worker_ref::register_shutdown).
// Linking and unlinking are O(1) under the registry mutex and never allocate.
// The link keeps the registry alive, so destruction unlinks even after the
// owner released its own worker_ref, on any thread. Declare it as the
// owner's first member so it unlinks only after every other member has
// been destroyed and no phase can observe a half-destroyed owner.
class worker_shutdown_registration final {
public:
    worker_shutdown_registration() noexcept = default;
    worker_shutdown_registration(const worker_shutdown_registration&) = delete;
    worker_shutdown_registration& operator=(const worker_shutdown_registration&) = delete;
    worker_shutdown_registration(worker_shutdown_registration&&) = delete;
    worker_shutdown_registration& operator=(worker_shutdown_registration&&) = delete;
    ~worker_shutdown_registration();

private:
    friend class detail::worker_dispatcher;

    const detail::worker_shutdown_operations_type* operations_{nullptr};
    std::shared_ptr<detail::worker_dispatcher> registry_;
    // Aliases the owner object; locking it pins the owner, and therefore this
    // registration, for the duration of one phase callback.
    std::weak_ptr<void> owner_;
    worker_shutdown_registration* previous_{nullptr};
    worker_shutdown_registration* next_{nullptr};
    bool linked_{false};
};

class worker_ref final {
public:
    worker_ref() noexcept = default;

    // The worker whose event loop runs on the calling thread; empty elsewhere.
    [[nodiscard]] static worker_ref current() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] bool is_current() const noexcept;
    [[nodiscard]] worker_id_type id() const noexcept;

    // The blocking pool the worker's event loop uses, or null (always null for
    // an empty worker_ref); run_blocking() on a worker without one reports
    // blocking_status::unavailable. The pool's owner keeps it alive for at
    // least the worker's lifetime.
    [[nodiscard]] ruvia::blocking_pool* blocking_pool() const noexcept;

    // Links `owner->*registration` into the worker's shutdown registry, so the
    // worker's stop reaches the owner's worker_stopping() and
    // worker_stopping_complete() phases on the stopping thread. The registry
    // holds only a weak reference: an owner already being destroyed is skipped,
    // and destroying the registration unlinks it. Returns false, linking
    // nothing, once the worker no longer accepts work (or on an empty
    // worker_ref). Linking an already registered member throws std::logic_error;
    // a null owner or member throws std::invalid_argument.
    template <worker_shutdown_observer owner_type>
    [[nodiscard]] bool register_shutdown(const std::shared_ptr<owner_type>& owner, worker_shutdown_registration std::type_identity_t<owner_type>::* registration) const {
        if (owner == nullptr || registration == nullptr) {
            throw std::invalid_argument("worker shutdown registration requires an owner member");
        }
        return link_shutdown((*owner).*registration, std::weak_ptr<void>(owner), detail::worker_shutdown_operations<owner_type>);
    }

    // The general bounded post. Reserves admission before synchronously
    // invoking `factory`, so the caller's inputs are consumed only after
    // admission: a refusal (queue_full, or stopping, also for an empty
    // worker_ref) never calls it. Keeps the selected endpoint alive across
    // factory code, even if it reentrantly retires the runtime and clears the
    // caller's worker_ref. An empty factory throws std::invalid_argument
    // before admission, whatever the worker's state. An admitted factory that
    // throws, or that produces an empty callable (std::invalid_argument),
    // releases the reservation. accepted does not guarantee execution: detaching
    // the runtime abandons an admitted callback and releases its owned inputs.
    [[nodiscard]] post_status post_factory(move_only_function<move_only_function<void()>()> factory) const;

    // post_factory() for a typed payload: `wrap` turns the payload into the
    // posted callable only after admission, and a refusal returns the
    // untouched payload in the result.
    template <typename payload_type, typename wrap_type>
        requires std::invocable<wrap_type&, payload_type&&> && std::convertible_to<std::invoke_result_t<wrap_type&, payload_type&&>, move_only_function<void()>>
    [[nodiscard]] delivery_result<payload_type, post_status, post_status::accepted> post_payload(payload_type payload, wrap_type wrap) const {
        using result_type = delivery_result<payload_type, post_status, post_status::accepted>;
        const auto status = post_factory([&]() -> move_only_function<void()> {
            return wrap(std::move(payload));
        });
        if (status != post_status::accepted) {
            return detail::delivery_access::reject<result_type>(status, std::move(payload));
        }
        return detail::delivery_access::accept<result_type>();
    }

    // post_factory() for a callable that is already built: a refusal returns
    // it untouched in the result. An empty callable throws
    // std::invalid_argument before admission, whatever the worker's state.
    template <typename fn_type>
        requires detail::move_only_function_target<void, fn_type>
    [[nodiscard]] post_result<> post(fn_type&& fn) const {
        if constexpr (detail::move_only_function_borrow_safe_input<void(), fn_type>) {
            return post_task(move_only_function<void()>(std::forward<fn_type>(fn)));
        } else {
            // Snapshot the endpoint before invoking any user-controlled construction/move.
            const worker_ref snapshot = *this;
            return snapshot.post_task(move_only_function<void()>(std::forward<fn_type>(fn)));
        }
    }

    // Resumes `continuation` on the worker in a later turn, never inline --
    // for a coroutine suspended in an operation this worker already runs.
    void defer_resume(std::coroutine_handle<> continuation) const noexcept;

    // The completion lane of an operation the worker already admitted:
    // cancellation, cleanup, or result delivery.
    [[nodiscard]] bool defer_completion(move_only_function<void()> completion) const;

private:
    explicit worker_ref(std::shared_ptr<detail::worker_dispatcher> dispatcher) noexcept;
    [[nodiscard]] post_result<> post_task(move_only_function<void()> task) const;
    [[nodiscard]] bool link_shutdown(worker_shutdown_registration& registration, std::weak_ptr<void> owner, const detail::worker_shutdown_operations_type& operations) const;

    std::shared_ptr<detail::worker_dispatcher> dispatcher_;
    friend class detail::worker_dispatcher;
};

namespace detail {

// What core's own inline headers need beyond the public worker_ref surface.
struct worker_ref_access {
    static void defer_or_terminate(const worker_ref& worker, move_only_function<void()> task) noexcept;
};

}  // namespace detail

}  // namespace ruvia
