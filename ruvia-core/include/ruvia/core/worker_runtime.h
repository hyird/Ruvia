#pragma once

#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <vector>

#include <asio/io_context.hpp>

#include "ruvia/core/move_only_function.h"
#include "ruvia/core/worker_ref.h"

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
