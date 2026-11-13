#pragma once

#include <chrono>
#include <concepts>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio/io_context.hpp>

#include "ruvia/core/worker_timer.h"

namespace ruvia {

// 64-bit FNV-1a parameters (non-cryptographic; dispatch and fingerprint use only).
inline constexpr std::uint64_t fnv1a_offset_basis = 14695981039346656037ULL;
inline constexpr std::uint64_t fnv1a_prime = 1099511628211ULL;

[[nodiscard]] constexpr std::uint64_t fnv1a_accumulate(std::uint64_t hash, std::string_view bytes) noexcept {
    for (const char byte : bytes) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= fnv1a_prime;
    }
    return hash;
}

// Mixes an integer as its little-endian bytes so results do not depend on host order.
[[nodiscard]] constexpr std::uint64_t fnv1a_accumulate_integer(std::uint64_t hash, std::uint64_t value) noexcept {
    for (std::size_t i = 0; i < sizeof(value); ++i) {
        hash ^= (value >> (i * 8U)) & 0xffU;
        hash *= fnv1a_prime;
    }
    return hash;
}

}  // namespace ruvia

namespace ruvia {
template <typename signature_type>
class move_only_function;
}  // namespace ruvia

namespace ruvia::detail {
template <typename t_type>
class worker_wait_value final {
public:
    template <typename u_type>
        requires std::constructible_from<t_type, u_type&&>
    explicit worker_wait_value(u_type&& value) noexcept(std::is_nothrow_constructible_v<t_type, u_type&&>)
        : value_(std::forward<u_type>(value)) {}

    t_type value_;
};

template <typename t_type>
inline constexpr bool is_move_only_function = false;
template <typename signature_type>
inline constexpr bool is_move_only_function<::ruvia::move_only_function<signature_type>> = true;

struct worker_delivery_access;
}  // namespace ruvia::detail

namespace ruvia {

enum class worker_wait_status : std::uint8_t {
    value,
    closed,
    // The bound worker is shutting down: its stopping notification arrived, or
    // a timed wait found (or was pending when) the worker's timer queue stopped.
    worker_stopping,
    timed_out,
    cancelled,
};

// Worker-bound waits have five mutually exclusive outcomes. A status is always
// available for exhaustive handling; only value owns a payload.
// The outcome category is fixed at construction. Construction and extraction
// follow the payload's copy/move semantics; a failed payload move never leaves
// an existing outcome without a category. Rebuild an owning optional to replace
// an outcome instead of assigning it. with_value() and with_status() let a
// composed worker-bound wait produce the same outcomes as the built-in ones.
template <typename t_type>
class worker_wait_result final {
public:
    worker_wait_result(const worker_wait_result&) = default;
    worker_wait_result(worker_wait_result&&) = default;
    worker_wait_result& operator=(const worker_wait_result&) = delete;
    worker_wait_result& operator=(worker_wait_result&&) = delete;

    [[nodiscard]] worker_wait_status status() const noexcept {
        return std::holds_alternative<detail::worker_wait_value<t_type>>(result_)
                   ? worker_wait_status::value
                   : std::get<worker_wait_status>(result_);
    }

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<detail::worker_wait_value<t_type>>(result_);
    }

    [[nodiscard]] const t_type& value() const& {
        const auto* result_value = std::get_if<detail::worker_wait_value<t_type>>(&result_);
        if (result_value == nullptr) {
            throw std::logic_error("worker wait result has no value");
        }
        return result_value->value_;
    }
    const t_type& value() const&& = delete;

    [[nodiscard]] t_type& value() & {
        auto* result_value = std::get_if<detail::worker_wait_value<t_type>>(&result_);
        if (result_value == nullptr) {
            throw std::logic_error("worker wait result has no value");
        }
        return result_value->value_;
    }
    t_type& value() && = delete;

    [[nodiscard]] t_type take_value() && {
        auto* result_value = std::get_if<detail::worker_wait_value<t_type>>(&result_);
        if (result_value == nullptr) {
            throw std::logic_error("worker wait result has no value");
        }
        return std::move(result_value->value_);
    }

    template <typename u_type>
        requires std::constructible_from<t_type, u_type&&>
    [[nodiscard]] static worker_wait_result with_value(u_type&& value) noexcept(
        std::is_nothrow_constructible_v<t_type, u_type&&>) {
        return worker_wait_result(std::in_place_type<detail::worker_wait_value<t_type>>, std::forward<u_type>(value));
    }

    // Every status except value; value outcomes come only from with_value().
    [[nodiscard]] static worker_wait_result with_status(worker_wait_status status) {
        if (status == worker_wait_status::value) {
            throw std::invalid_argument("worker wait status result requires a non-value status");
        }
        return worker_wait_result(status);
    }

private:
    using result_type = std::variant<detail::worker_wait_value<t_type>, worker_wait_status>;

    template <typename u_type>
    explicit worker_wait_result(
        std::in_place_type_t<detail::worker_wait_value<t_type>>, u_type&& value) noexcept(std::is_nothrow_constructible_v<t_type, u_type&&>)
        : result_(std::in_place_type<detail::worker_wait_value<t_type>>, std::forward<u_type>(value)) {}

    explicit worker_wait_result(worker_wait_status status) noexcept
        : result_(status) {}

    result_type result_;
};

// The outcome of a bounded delivery: post, submit, send, complete, or start.
// The category is fixed at construction. A rejection owns the unconsumed
// payload; when accepted_type is not void, acceptance owns that value (the
// started operation). take_rejected() and take_value() both consume their
// payload. Only the delivering primitive produces results, so an accepted
// result always reflects a real admission.
template <typename t_type, typename status_type, status_type accepted_status, typename accepted_type = void>
class worker_delivery_result final {
    using accepted_storage_type = std::conditional_t<std::is_void_v<accepted_type>, std::monostate, accepted_type>;

public:
    worker_delivery_result(const worker_delivery_result&) = delete;
    worker_delivery_result& operator=(const worker_delivery_result&) = delete;
    worker_delivery_result(worker_delivery_result&&) noexcept(
        std::is_nothrow_move_constructible_v<t_type> && std::is_nothrow_move_constructible_v<accepted_storage_type>) =
        default;
    worker_delivery_result& operator=(worker_delivery_result&&) = delete;

    [[nodiscard]] status_type status() const noexcept {
        return status_;
    }

    [[nodiscard]] bool accepted() const noexcept {
        return status_ == accepted_status;
    }

    template <typename self_type = accepted_type>
        requires(!std::is_void_v<self_type>)
    [[nodiscard]] self_type& value() & {
        return require_accepted();
    }
    template <typename self_type = accepted_type>
        requires(!std::is_void_v<self_type>)
    [[nodiscard]] const self_type& value() const& {
        return const_cast<worker_delivery_result&>(*this).require_accepted();
    }
    template <typename self_type = accepted_type>
        requires(!std::is_void_v<self_type>)
    self_type& value() && = delete;
    template <typename self_type = accepted_type>
        requires(!std::is_void_v<self_type>)
    const self_type& value() const&& = delete;

    template <typename self_type = accepted_type>
        requires(!std::is_void_v<self_type>)
    [[nodiscard]] self_type take_value() && {
        auto& accepted_value = require_accepted();
        struct clear_accepted final {
            std::optional<accepted_storage_type>& accepted_;
            ~clear_accepted() {
                accepted_.reset();
            }
        } clear{accepted_};
        return std::move(accepted_value);
    }

    [[nodiscard]] t_type* rejected() & noexcept {
        return rejected_ ? std::addressof(*rejected_) : nullptr;
    }

    [[nodiscard]] const t_type* rejected() const& noexcept {
        return rejected_ ? std::addressof(*rejected_) : nullptr;
    }

    t_type* rejected() && = delete;
    const t_type* rejected() const&& = delete;

    // A failed extraction retains the payload, in the state its move left it.
    // Only successful return-value construction consumes the rejected value.
    [[nodiscard]] t_type take_rejected() && {
        if (status_ == accepted_status || !rejected_) {
            throw std::logic_error("delivery result has no rejected value");
        }
        struct clear_rejected final {
            std::optional<t_type>& rejected_;
            const int exceptions_{std::is_nothrow_move_constructible_v<t_type> ? 0 : std::uncaught_exceptions()};
            ~clear_rejected() {
                if (std::is_nothrow_move_constructible_v<t_type> || std::uncaught_exceptions() == exceptions_) {
                    rejected_.reset();
                }
            }
        } clear{rejected_};
        return std::move(*rejected_);
    }

    friend bool operator==(const worker_delivery_result& outcome, status_type status) noexcept {
        return outcome.status_ == status;
    }
    friend bool operator==(status_type status, const worker_delivery_result& outcome) noexcept {
        return outcome == status;
    }

private:
    friend struct detail::worker_delivery_access;

    explicit worker_delivery_result(status_type status) noexcept
        : status_(status) {}

    worker_delivery_result(status_type status, std::in_place_t, accepted_storage_type&& accepted_value) noexcept(
        std::is_nothrow_move_constructible_v<accepted_storage_type>)
        : status_(status),
          accepted_(std::move(accepted_value)) {}

    worker_delivery_result(status_type status, t_type&& rejected) noexcept(
        std::is_nothrow_move_constructible_v<t_type>)
        : status_(status),
          rejected_(std::move(rejected)) {}

    [[nodiscard]] accepted_storage_type& require_accepted() {
        if (status_ != accepted_status || !accepted_) {
            throw std::logic_error("delivery result has no accepted value");
        }
        return *accepted_;
    }

    status_type status_;
    std::optional<t_type> rejected_;
    [[no_unique_address]] std::conditional_t<std::is_void_v<accepted_type>, std::monostate,
        std::optional<accepted_storage_type>>
        accepted_;
};

namespace detail {

// The producer side of worker_delivery_result, reserved to the delivering
// primitives so callers cannot fabricate an accepted outcome.
struct worker_delivery_access final {
    template <typename result_type>
    [[nodiscard]] static result_type accept() noexcept {
        return result_type(accepted_status_of<result_type>());
    }

    template <typename result_type, typename accepted_value_type>
    [[nodiscard]] static result_type accept(accepted_value_type&& accepted_value) noexcept(
        std::is_nothrow_move_constructible_v<std::remove_cvref_t<accepted_value_type>>) {
        return result_type(
            accepted_status_of<result_type>(), std::in_place, std::forward<accepted_value_type>(accepted_value));
    }

    template <typename result_type, typename status_type, typename rejected_type>
    [[nodiscard]] static result_type reject(status_type status, rejected_type&& value) {
        if (status == accepted_status_of<result_type>()) {
            throw std::invalid_argument("rejected delivery result requires a rejection status");
        }
        if constexpr (is_move_only_function<std::remove_cvref_t<rejected_type>>) {
            if (!value) {
                throw std::invalid_argument("rejected delivery result requires a callable task");
            }
        }
        return result_type(status, std::forward<rejected_type>(value));
    }

private:
    template <typename result_type>
    struct accepted_status_traits;

    template <typename t_type, typename status_type, status_type accepted_status, typename accepted_type>
    struct accepted_status_traits<worker_delivery_result<t_type, status_type, accepted_status, accepted_type>> final {
        static constexpr status_type value = accepted_status;
    };

    template <typename result_type>
    [[nodiscard]] static constexpr auto accepted_status_of() noexcept {
        return accepted_status_traits<result_type>::value;
    }
};

}  // namespace detail

}  // namespace ruvia

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
struct worker_ref_access;

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

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] bool is_current() const noexcept;
    [[nodiscard]] worker_id_type id() const noexcept;
    // The blocking pool this worker's event loop borrows, or null; run_blocking()
    // on a worker without one reports blocking_status::unavailable. The pool's
    // owner keeps it alive for at least the worker's lifetime.
    [[nodiscard]] ruvia::blocking_pool* blocking_pool() const noexcept;

    // Native I/O integration, not bounded business admission. The executor
    // borrows the runtime's io_context: finish its operations before the
    // runtime/attachment retires and never retain it beyond context destruction.
    // Empty or detached worker refs throw std::logic_error; stopping workers still
    // expose their executor so already admitted operations can finish.
    [[nodiscard]] asio::io_context::executor_type executor() const;

    // Worker-affine timer registration. This stable worker_ref must outlive the
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
    // and clears this worker_ref. accepted does not guarantee execution: detaching
    // the runtime abandons an admitted callback and releases its owned inputs.
    [[nodiscard]] post_status post_factory(move_only_function<move_only_function<void()>()> factory) const;

    // post_factory() for a typed payload: `wrap` turns the payload into the
    // posted callable only after admission, and a refusal returns the
    // untouched payload in the result. An empty worker_ref refuses with
    // worker_stopping.
    template <typename payload_type, typename wrap_type>
        requires std::invocable<wrap_type&, payload_type&&> &&
                 std::convertible_to<std::invoke_result_t<wrap_type&, payload_type&&>, move_only_function<void()>>
    [[nodiscard]] worker_delivery_result<payload_type, post_status, post_status::accepted> post_payload(
        payload_type payload, wrap_type wrap) const {
        using result_type = worker_delivery_result<payload_type, post_status, post_status::accepted>;
        const auto status = post_factory([&]() -> move_only_function<void()> { return wrap(std::move(payload)); });
        if (status != post_status::accepted) {
            return detail::worker_delivery_access::reject<result_type>(status, std::move(payload));
        }
        return detail::worker_delivery_access::accept<result_type>();
    }

    template <typename fn_type>
        requires detail::move_only_function_target<void, fn_type>
    [[nodiscard]] post_result_type post(fn_type&& fn) const {
        if constexpr (detail::move_only_function_borrow_safe_input<void(), fn_type>) {
            return post_task(move_only_function<void()>(std::forward<fn_type>(fn)));
        } else {
            // Snapshot the endpoint before invoking any user-controlled construction/move.
            const worker_ref snapshot = *this;
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
    // the worker's execution context has been detached, or on an empty worker_ref,
    // it terminates. A request the event loop is destroyed before
    // running is dropped and the coroutine never resumes, so the worker must
    // run until every operation it admitted has completed, as for any started
    // task.
    void defer_resume(std::coroutine_handle<> continuation) const noexcept;

    // The completion lane of an operation this worker already admitted:
    // cancellation, cleanup, or result delivery. It takes no retirement
    // protection of its own; loop-level cleanup that belongs to no admitted
    // operation uses event_loop::defer_cleanup() instead. Unlike post() a full queue
    // never refuses it; on the worker's own thread it runs in a later turn,
    // never inline. Returns false, destroying `completion` on the caller's
    // thread, once the worker's execution context has been detached (or on an
    // empty worker_ref): the admitted operation has then retired with
    // its worker. Throws std::invalid_argument for an empty callable.
    [[nodiscard]] bool defer_completion(move_only_function<void()> completion) const;

    // Links `owner->*registration` into this worker's shutdown registry, so
    // the worker's stop reaches the owner's worker_stopping() and
    // worker_stopping_complete() phases on the stopping thread. The registry
    // holds only a weak reference: an owner already being destroyed is
    // skipped, and destroying the registration unlinks it. Returns
    // worker_stopping, linking nothing, once the worker no longer accepts
    // work (or on an empty worker_ref). Linking an already registered
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
    explicit worker_ref(std::shared_ptr<detail::worker_dispatcher> dispatcher) noexcept;
    [[nodiscard]] post_result_type post_task(move_only_function<void()> task) const;
    [[nodiscard]] worker_registration_status link_shutdown(worker_shutdown_registration& registration,
        std::weak_ptr<void> owner, const detail::worker_shutdown_operations_type& operations) const;

    // Owns the stable dispatcher endpoint, not the worker's io_context.
    // The worker detaches that endpoint before destroying its execution context.
    std::shared_ptr<detail::worker_dispatcher> dispatcher_;
    friend struct detail::worker_ref_access;
};

namespace detail {

struct worker_ref_access {
    [[nodiscard]] static worker_ref make(
        const std::shared_ptr<worker_dispatcher>& dispatcher) noexcept;
    static void defer_or_terminate(
        const worker_ref& worker, move_only_function<void()> task) noexcept;
    static void when_shutdown_notifications_complete(
        const worker_ref& worker, move_only_function<void()> callback);
    static void when_idle(const worker_ref& worker, move_only_function<void()> callback);
    static void wait_for_reservations(const worker_ref& worker) noexcept;
    [[nodiscard]] static bool timers_stopping(const worker_ref& worker) noexcept;
    static void enable_external_runner(const worker_ref& worker) noexcept;
    // The worker whose event loop runs on the calling thread; empty elsewhere.
    [[nodiscard]] static worker_ref current() noexcept;
};

}  // namespace detail

}  // namespace ruvia

namespace ruvia {

enum class worker_runtime_state : std::uint8_t {
    ready,
    running,
    stopping,
    stopped,
};

class blocking_pool;
class event_loop;
class worker_group;
class worker_runtime_context;

namespace detail {
struct event_loop_state;
}

enum class worker_io_policy : std::uint8_t { synchronized,
    single_owner };

struct worker_runtime_options final {
    std::size_t queue_capacity_{4096};
    worker_io_policy io_policy_{worker_io_policy::synchronized};
    // Borrowed pool for run_blocking() on this worker; it must outlive the
    // runtime. Null leaves the worker without one.
    blocking_pool* blocking_pool_{nullptr};
};

struct worker_runtime_hooks final {
    // Owner thread, once, at launch. Begins startup; the owner publishes
    // readiness with mark_ready() once its (possibly asynchronous) startup work
    // has completed.
    move_only_function<void()> startup_{};
    // Owner thread, once, after request_serve() follows readiness. Serving is
    // published when it returns; a throw fails the runtime.
    move_only_function<void()> serve_{};
    // Runs on the owner after dispatcher admission and shutdown notifications
    // close. A custom policy must eventually call finalize(), after its external
    // producers are quiescent. Without a policy, stopping finalizes immediately.
    move_only_function<void()> stop_admission_{};
    // Owner thread, once, when finalization releases the work guard. Retires
    // domain resources; pending I/O and continuations still drain afterwards.
    // During finalization, its target's move and destruction may reenter finalize().
    move_only_function<void()> finalize_{};
    // Runs once, for the first failure only, on the thread that reported it:
    // the owner for escaped failures, the caller for fail().
    move_only_function<void(std::exception_ptr)> failure_{};
    // Terminal owner-thread cleanup, after I/O and completion handlers drain.
    move_only_function<void()> shutdown_{};
};

// The unique owner of a worker's thread, io_context, dispatcher, and generic
// startup/serve/stop/drain/join lifecycle, including its first failure.
// Protocol/application state is composed via hooks; it must remain alive
// through join(). No hook may throw except startup_ and serve_.
class worker_runtime final {
    class impl_type;

public:
    explicit worker_runtime(worker_runtime_options options = {});
    ~worker_runtime();

    worker_runtime(const worker_runtime&) = delete;
    worker_runtime& operator=(const worker_runtime&) = delete;
    worker_runtime(worker_runtime&&) = delete;
    worker_runtime& operator=(worker_runtime&&) = delete;

    // Configure once, before publishing the runtime or starting/stopping it.
    // Hook target moves may call take_failure() during configuration.
    void configure(worker_runtime_hooks hooks);
    // Launches the owner thread. Returns false when a stop already closed the
    // runtime before launch; a second start throws std::logic_error.
    [[nodiscard]] bool start();
    // Two-phase startup barrier. Owner-published readiness; a stop first aborts
    // it. wait_until_ready() returns false once stopped before readiness and
    // rethrows the first failure. request_serve() requires readiness and runs
    // serve_ on the owner; wait_until_serving() mirrors wait_until_ready().
    void mark_ready() noexcept;
    [[nodiscard]] bool wait_until_ready();
    void request_serve();
    [[nodiscard]] bool wait_until_serving();
    // Every call closes dispatcher admission before returning. Repeated or
    // reentrant requests do not wait for the winning stop dispatch to finish.
    void request_stop() noexcept;
    // Records the first failure, wakes phase waiters, and requests stop. Only
    // the winning failure reaches the failure_ hook. Returns true for it.
    bool fail(std::exception_ptr failure) noexcept;
    // Cold-path lifecycle control, serialized with finalization. Returns false
    // once stopping starts; it cannot strand a control after the thread barrier.
    [[nodiscard]] bool post_control(move_only_function<void()> control) noexcept;
    // A reliable owner-thread phase-two control; the first call wins. The
    // finalize_ hook retires domain resources before the work guard is
    // released. Pending I/O/continuations still drain; the io_context is never
    // stopped.
    void finalize() noexcept;
    // Establishes the thread barrier, including owner-affine draining when
    // stopped before start. Concurrent joins wait for the same barrier. This
    // does not rethrow run failures: inspect/rethrow them after all owners join.
    // Waits for any external stop dispatch before draining a cold owner. Joining
    // from its owner or from a thread currently dispatching its stop hooks
    // throws logic_error instead of waiting on itself.
    void join();
#if defined(_WIN32)
    class thread_join_operation final {
    public:
        thread_join_operation(const thread_join_operation&) = delete;
        thread_join_operation& operator=(const thread_join_operation&) = delete;
        void operator&() const = delete;
        ~thread_join_operation();
        [[nodiscard]] bool await_ready();
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation);
        [[nodiscard]] std::uintptr_t await_resume();

    private:
        thread_join_operation(worker_runtime& owner, worker_ref caller) noexcept;
        worker_runtime* owner_;
        worker_ref caller_;
        thread_join_operation* next_{};
        std::coroutine_handle<> continuation_{};
        bool started_{};
        bool resumed_{};
        friend class worker_runtime;
        friend class impl_type;
    };
    // Await on caller: requests stop and joins any external stop dispatch
    // without blocking its event loop. Launches a cold owner's drain thread
    // if needed, then returns an owned SYNCHRONIZE HANDLE as uintptr_t.
    // Wait asynchronously for that HANDLE, close it, and call join() to reap
    // the exited thread. Zero means already joined. Runtime must outlive the
    // operation; concurrent join() is not permitted.
    [[nodiscard]] thread_join_operation prepare_thread_join(worker_ref caller) &;
    thread_join_operation prepare_thread_join(worker_ref) && = delete;
#endif
    // Takes reporting ownership of the first failure, if any: destruction no
    // longer reports a failure taken here. Later calls return the same failure.
    [[nodiscard]] std::exception_ptr take_failure() noexcept;
    [[nodiscard]] worker_runtime_state state() const noexcept;
    [[nodiscard]] asio::io_context& io_context() & noexcept;
    asio::io_context& io_context() && = delete;
    [[nodiscard]] const worker_ref& worker() const& noexcept;
    const worker_ref& worker() const&& = delete;
    // The event loop over this runtime's worker, created on first use, for
    // try_start()/start(), on_stop(), defer_cleanup() and report_failure()
    // (which fails this runtime). Once a loop exists, the runtime's stop
    // admission waits for the loop's root tasks, stop callbacks, and cleanup
    // to drain before stop_admission_ (or the default finalize) runs. The loop
    // borrows this runtime: no copy of it may outlive the runtime.
    [[nodiscard]] event_loop loop() &;
    event_loop loop() && = delete;

private:
    friend class worker_group;
    friend struct detail::event_loop_state;
    [[nodiscard]] worker_runtime_context& context() & noexcept;
    // The first failure without taking ownership of it, for comparisons.
    [[nodiscard]] std::exception_ptr recorded_failure() const noexcept;
    void bind_group(worker_group& group);
    void unbind_group(worker_group& group) noexcept;
    // Owner thread: the loop's retirement gate has drained; continue the stop
    // admission the gate deferred.
    void finish_loop_stop_admission() noexcept;

    std::unique_ptr<impl_type> impl_;
};

}  // namespace ruvia

namespace ruvia {

class worker_runtime;

// Sequences a set of worker_runtime owners through one lifecycle:
// - start(): launches every member and waits until all are ready;
// - serve(): requests serving on every member and waits until all are serving;
// - request_stop(): closes admission on every member at once; lifecycle hooks
//   may query stop_requested() or repeat request_stop();
// - join(): requests stop, waits for stop dispatch, and joins every member.
// Member failure closes admission on the entire group and wakes pending phase
// waits, preserving the original exception. An independently stopped member
// aborts the group; a group stop instead makes its phase calls return false.
// The group borrows its members, which must outlive it. Its destruction removes
// and synchronizes member observations; callers still join the group explicitly
// to retire its members. All calls except request_stop() and stop_requested()
// belong to one lifecycle thread.
class worker_group final {
public:
    worker_group();
    ~worker_group();

    worker_group(const worker_group&) = delete;
    worker_group& operator=(const worker_group&) = delete;

    // Cold setup before start() or stop. Each member belongs to one group and
    // may be added once.
    void add(worker_runtime& member);
    // Launches every member without waiting for readiness; a thread creation
    // failure stops and joins the group before rethrowing.
    void launch();
    // Phase one. Returns false when the group was stopped first; rethrows the
    // first member failure; throws std::runtime_error when a member stopped on
    // its own before readiness.
    [[nodiscard]] bool start();
    // Phase two, after start() succeeded. Same outcome rules as start().
    [[nodiscard]] bool serve();
    void request_stop() noexcept;
    [[nodiscard]] bool stop_requested() const noexcept;
    // Requests stop and waits for its member notifications to finish before
    // joining every member. Returns the failure its caller rethrows:
    // `primary` when given (a failure the caller already holds, such as one
    // start() or serve() threw), else the first member failure in registration
    // order. It is chosen before any report, so it is never reported here;
    // every other member failure is reported as unhandled ("additional worker"),
    // equivalent ones collapsed into one line with their count. Repeated joins
    // return the first result without repeating retirement or diagnostics.
    [[nodiscard]] std::exception_ptr join(std::exception_ptr primary = nullptr) noexcept;

private:
    friend class worker_runtime;
    [[nodiscard]] bool launch(worker_runtime& member);
    [[nodiscard]] bool settle(bool reached, const char* phase);
    void member_failed(worker_runtime& member, std::exception_ptr failure) noexcept;
    void member_stopping() noexcept;
    void wait_for_stop_dispatch() noexcept;
    [[nodiscard]] std::exception_ptr observe_phase_failure() noexcept;
    [[noreturn]] void rethrow_phase_failure(std::exception_ptr fallback);

    // Borrowed members in registration order.
    std::vector<worker_runtime*> members_;
    mutable std::mutex mutex_;
    std::condition_variable stop_changed_;
    bool stopping_{false};
    bool stop_dispatching_{false};
    bool sealed_{false};
    bool ready_{false};
    bool serve_requested_{false};
    bool member_stopped_{false};
    std::exception_ptr failure_;
    worker_runtime* failed_member_{nullptr};
    bool joining_{false};
    bool joined_{false};
    std::exception_ptr join_failure_;
};

}  // namespace ruvia
