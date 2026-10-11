#pragma once

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <memory_resource>
#include <mutex>
#include <vector>

#include "ruvia/core/worker_runtime.h"

namespace ruvia {

// Sequences a set of worker_runtime owners through one lifecycle. Members are
// grouped in tiers, added in non-decreasing tier order:
// - start(): each tier launches and becomes ready before the next tier
//   launches, so an earlier tier may stage state into a later one;
// - serve(): serving is requested from the last tier back to the first, so no
//   tier admits work before everything downstream of it serves;
// - request_stop(): closes admission on every member at once; member abandon
//   hooks may query stop_requested() or repeat request_stop();
// - join(): joins tiers in order. Earlier tiers are the external producers of
//   later ones: each later tier is finalized only once every earlier tier has
//   joined, while first-tier members finalize under their own stop policy.
// Member failure closes admission on the entire group and wakes pending phase
// waits, preserving the original exception. An independently stopped member
// aborts the group; a group stop instead makes its phase calls return false.
// The group borrows its members, which must outlive it. Its destruction removes
// and synchronizes member observations; callers still join the group explicitly
// to retire its tiers in order. All calls except request_stop() and
// stop_requested() belong to one lifecycle thread.
class worker_group final {
public:
    explicit worker_group(std::pmr::memory_resource* resource = nullptr);
    ~worker_group();

    worker_group(const worker_group&) = delete;
    worker_group& operator=(const worker_group&) = delete;

    // Cold setup before start() or stop. Each member belongs to one group and
    // may be added once. `tier` must equal the last tier or follow it.
    void add(worker_runtime& member, std::size_t tier);
    // Phase one. Returns false when the group was stopped first; rethrows the
    // first member failure; throws std::runtime_error when a member stopped on
    // its own before readiness.
    [[nodiscard]] bool start();
    // Phase two, after start() succeeded. Same outcome rules as start().
    [[nodiscard]] bool serve();
    void request_stop() noexcept;
    [[nodiscard]] bool stop_requested() const noexcept;
    // Requests stop and waits for its member notifications/abandon hooks to
    // finish before retiring any tier. Returns the first member failure in
    // tier order; later ones are reported as unhandled. Repeated joins return
    // the same result without repeating retirement or diagnostics.
    [[nodiscard]] std::exception_ptr join() noexcept;

private:
    friend class worker_runtime;
    struct member_type final {
        worker_runtime* runtime_;
        std::size_t tier_;
    };

    [[nodiscard]] bool launch(worker_runtime& member);
    [[nodiscard]] bool settle(bool reached, const char* phase);
    void member_failed(worker_runtime& member, std::exception_ptr failure) noexcept;
    void member_stopping() noexcept;
    void wait_for_stop_dispatch() noexcept;
    [[nodiscard]] std::exception_ptr observe_phase_failure() noexcept;
    [[noreturn]] void rethrow_phase_failure(std::exception_ptr fallback);

    std::pmr::vector<member_type> members_;
    mutable std::mutex mutex_;
    std::condition_variable stop_changed_;
    bool stopping_{false};
    bool stop_dispatching_{false};
    bool sealed_{false};
    bool member_stopped_{false};
    std::exception_ptr failure_;
    worker_runtime* failed_member_{nullptr};
    bool joined_{false};
    std::exception_ptr join_failure_;
};

}  // namespace ruvia
