#pragma once

#include <condition_variable>
#include <exception>
#include <memory_resource>
#include <mutex>
#include <vector>

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
    explicit worker_group(std::pmr::memory_resource* resource = nullptr);
    ~worker_group();

    worker_group(const worker_group&) = delete;
    worker_group& operator=(const worker_group&) = delete;

    // Cold setup before start() or stop. Each member belongs to one group and
    // may be added once.
    void add(worker_runtime& member);
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
    // Borrowed members in registration order.

    [[nodiscard]] bool launch(worker_runtime& member);
    [[nodiscard]] bool settle(bool reached, const char* phase);
    void member_failed(worker_runtime& member, std::exception_ptr failure) noexcept;
    void member_stopping() noexcept;
    void wait_for_stop_dispatch() noexcept;
    [[nodiscard]] std::exception_ptr observe_phase_failure() noexcept;
    [[noreturn]] void rethrow_phase_failure(std::exception_ptr fallback);

    std::pmr::vector<worker_runtime*> members_;
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
    bool joined_{false};
    std::exception_ptr join_failure_;
};

}  // namespace ruvia
