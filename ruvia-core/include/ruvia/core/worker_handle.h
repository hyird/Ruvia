#pragma once

#include <chrono>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <asio/io_context.hpp>

#include "ruvia/core/fnv1a.h"
#include "ruvia/core/move_only_function.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/core/worker_wait_result.h"

namespace ruvia {

using worker_id_type = std::uint64_t;

[[nodiscard]] constexpr std::uint64_t worker_selection_hash(std::string_view key) noexcept {
    return fnv1a_accumulate(fnv1a_offset_basis, key);
}

enum class post_status : std::uint8_t {
    accepted,
    queue_full,
    worker_stopping,
};

template <typename signature_type = void()>
using post_outcome =
    worker_delivery_result<move_only_function<signature_type>, post_status, post_status::accepted>;
using post_result_type = post_outcome<void()>;

class blocking_pool;

namespace detail {
class worker_dispatcher;
struct worker_handle_access;

// Per-owner-type phase table; a null entry skips that phase.
struct worker_shutdown_operations_type final {
    using phase_type = void (*)(void* owner) noexcept;

    phase_type stopping_;
    phase_type stopping_complete_;
};
}  // namespace detail

// An owner observes its worker's shutdown through `void worker_stopping()
// noexcept`, run in the first notification pass, and/or `void
// worker_stopping_complete() noexcept`, run in the second pass after every
// registered owner has seen the first.
template <typename owner_type>
concept worker_shutdown_observer =
    requires(owner_type& owner) {
        { owner.worker_stopping() } noexcept;
    } || requires(owner_type& owner) {
        { owner.worker_stopping_complete() } noexcept;
    };

namespace detail {

template <typename owner_type>
[[nodiscard]] consteval worker_shutdown_operations_type make_worker_shutdown_operations() noexcept {
    worker_shutdown_operations_type operations{nullptr, nullptr};
    if constexpr (requires(owner_type& owner) {
                      { owner.worker_stopping() } noexcept;
                  }) {
        operations.stopping_ = [](void* owner) noexcept { static_cast<owner_type*>(owner)->worker_stopping(); };
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
inline constexpr worker_shutdown_operations_type worker_shutdown_operations =
    make_worker_shutdown_operations<owner_type>();

}  // namespace detail

enum class worker_registration_status : std::uint8_t {
    registered,
    worker_stopping,
};

// Intrusive link of one shared owner into its worker's shutdown registry,
// held as a data member of that owner (see worker_handle::register_shutdown).
// Linking and unlinking are O(1) under the registry mutex and never allocate.
// The link keeps the registry alive, so destruction unlinks even after the
// owner released its own worker handle, on any thread. Declare it as the
// owner's first data member so it unlinks only after every other member has
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

class worker_handle final {
public:
    worker_handle() noexcept = default;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] bool is_current() const noexcept;
    [[nodiscard]] worker_id_type id() const noexcept;
    // Whether this worker's event loop was given a blocking pool; run_blocking()
    // on a worker without one reports blocking_status::unavailable.
    [[nodiscard]] bool has_blocking_pool() const noexcept;

    // Native I/O integration, not bounded business admission. The executor
    // borrows the runtime's io_context: finish its operations before the
    // runtime/attachment retires and never retain it beyond context destruction.
    // Empty or detached handles throw std::logic_error; stopping workers still
    // expose their executor so already admitted operations can finish.
    [[nodiscard]] asio::io_context::executor_type executor() const;

    // Worker-affine timer registration. This stable handle must outlive the
    // borrowed registration; destruction unregisters without a late callback.
    // Returns worker_stopping without storing the completion once this worker's
    // timer queue has stopped; callers that resume work from the completion
    // must handle it. Rescheduling a still-pending registration throws
    // std::logic_error.
    worker_timer_schedule_status schedule_timer(worker_timer_registration& registration,
        std::chrono::steady_clock::time_point deadline,
        move_only_function<void(worker_timer_outcome)> completion) const&;
    worker_timer_schedule_status schedule_timer(worker_timer_registration&,
        std::chrono::steady_clock::time_point,
        move_only_function<void(worker_timer_outcome)>) const&& = delete;

    // Reserves bounded admission before synchronously invoking the factory, so
    // ownership transfers only after admission. Keeps the selected endpoint
    // alive across user factory code, even if it reentrantly retires the runtime
    // and clears this handle. accepted does not guarantee execution: detaching
    // the runtime abandons an admitted callback and releases its owned inputs.
    [[nodiscard]] post_status post_factory(move_only_function<move_only_function<void()>()> factory) const;

    template <typename fn_type>
        requires detail::move_only_function_target<void, fn_type>
    [[nodiscard]] post_result_type post(fn_type&& fn) const {
        if constexpr (detail::move_only_function_borrow_safe_input<void(), fn_type>) {
            return post_task(move_only_function<void()>(std::forward<fn_type>(fn)));
        } else {
            // Snapshot the endpoint before invoking any user-controlled construction/move.
            const worker_handle snapshot = *this;
            return snapshot.post_task(move_only_function<void()>(std::forward<fn_type>(fn)));
        }
    }

    // Resumes `continuation` on this worker in a later turn, never inline --
    // for a coroutine suspended in an operation this worker already runs.
    // Requests made from one thread resume in the order they were made; there
    // is no order between requests made from different threads (on a worker
    // with a single owner, the worker's own requests reach the event loop only
    // after its current handler returns). Unlike post() it cannot be refused by
    // a full queue; on the worker's own thread it goes straight to the event
    // loop without a type-erased callable. Requested from another thread once
    // the worker's execution context has been detached, or on a handle without
    // a worker, it terminates. A request the event loop is destroyed before
    // running is dropped and the coroutine never resumes, so the worker must
    // run until every operation it admitted has completed, as for any started
    // task.
    void defer_resume(std::coroutine_handle<> continuation) const noexcept;

    // The completion lane of an operation this worker already admitted:
    // cancellation, cleanup, or result delivery. Unlike post() a full queue
    // never refuses it; on the worker's own thread it runs in a later turn,
    // never inline. Returns false, destroying `completion` on the caller's
    // thread, once the worker's execution context has been detached (or on a
    // handle without a worker): the admitted operation has then retired with
    // its worker. Throws std::invalid_argument for an empty callable.
    [[nodiscard]] bool defer_completion(move_only_function<void()> completion) const;

    // Links `owner->*registration` into this worker's shutdown registry, so
    // the worker's stop reaches the owner's worker_stopping() and
    // worker_stopping_complete() phases on the stopping thread. The registry
    // holds only a weak reference: an owner already being destroyed is
    // skipped, and destroying the registration unlinks it. Returns
    // worker_stopping, linking nothing, once the worker no longer accepts
    // work (or on a handle without a worker). Linking an already registered
    // member throws std::logic_error; a null owner or member throws
    // std::invalid_argument.
    template <typename owner_type>
        requires worker_shutdown_observer<owner_type>
    [[nodiscard]] worker_registration_status register_shutdown(
        const std::shared_ptr<owner_type>& owner, worker_shutdown_registration owner_type::* registration) const {
        if (owner == nullptr || registration == nullptr) {
            throw std::invalid_argument("worker shutdown registration requires an owner member");
        }
        return link_shutdown((*owner).*registration, std::weak_ptr<void>(owner),
            detail::worker_shutdown_operations<owner_type>);
    }

private:
    explicit worker_handle(std::shared_ptr<detail::worker_dispatcher> dispatcher) noexcept;
    [[nodiscard]] post_result_type post_task(move_only_function<void()> task) const;
    [[nodiscard]] worker_registration_status link_shutdown(worker_shutdown_registration& registration,
        std::weak_ptr<void> owner, const detail::worker_shutdown_operations_type& operations) const;

    // Owns the stable dispatcher endpoint, not the worker's io_context.
    // The worker detaches that endpoint before destroying its execution context.
    std::shared_ptr<detail::worker_dispatcher> dispatcher_;
    friend struct detail::worker_handle_access;
};

namespace detail {

struct worker_handle_access {
    [[nodiscard]] static worker_handle make(
        const std::shared_ptr<worker_dispatcher>& dispatcher) noexcept;
    static void defer_or_terminate(
        const worker_handle& worker, move_only_function<void()> task) noexcept;
    static void when_shutdown_notifications_complete(
        const worker_handle& worker, move_only_function<void()> callback);
    static void when_idle(const worker_handle& worker, move_only_function<void()> callback);
    static void wait_for_reservations(const worker_handle& worker) noexcept;
    [[nodiscard]] static bool timers_stopping(const worker_handle& worker) noexcept;
    static void enable_external_runner(const worker_handle& worker) noexcept;
    // The pool the worker's event loop borrows, or null.
    [[nodiscard]] static ruvia::blocking_pool* blocking_pool(const worker_handle& worker) noexcept;
};

}  // namespace detail

}  // namespace ruvia
