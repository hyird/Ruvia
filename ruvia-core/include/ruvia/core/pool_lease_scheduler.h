#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <optional>

#include "ruvia/core/pool_lease_release_status.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"

namespace ruvia {

// Public, self-contained outcome contract for an asynchronous pool acquire.
class pool_waiter_result final {
public:
    enum class status_type { acquired,
        timed_out,
        closed,
        cancelled };

    [[nodiscard]] constexpr status_type status() const noexcept {
        return status_;
    }
    [[nodiscard]] constexpr bool acquired() const noexcept {
        return status_ == status_type::acquired;
    }
    [[nodiscard]] constexpr std::size_t index() const noexcept {
        return index_;
    }

    [[nodiscard]] static constexpr pool_waiter_result make_acquired(std::size_t index) noexcept {
        return pool_waiter_result(status_type::acquired, index);
    }
    [[nodiscard]] static constexpr pool_waiter_result make_timed_out() noexcept {
        return pool_waiter_result(status_type::timed_out, 0);
    }
    [[nodiscard]] static constexpr pool_waiter_result make_closed() noexcept {
        return pool_waiter_result(status_type::closed, 0);
    }
    [[nodiscard]] static constexpr pool_waiter_result make_cancelled() noexcept {
        return pool_waiter_result(status_type::cancelled, 0);
    }

private:
    constexpr pool_waiter_result(status_type status, std::size_t index) noexcept
        : status_(status),
          index_(index) {}
    status_type status_;
    std::size_t index_;
};

// Single-worker owner of pool slot leases and their asynchronous wait queue.
// A supplied worker endpoint is retained once by the scheduler. Lazy acquires
// copy their inputs before return and borrow only their scheduler, which must
// outlive them.
class pool_lease_scheduler final {
public:
    explicit pool_lease_scheduler(std::size_t pool_size,
        std::pmr::memory_resource* resource = nullptr);
    pool_lease_scheduler(std::size_t pool_size, const worker_handle& worker_value,
        std::pmr::memory_resource* resource = nullptr);

    pool_lease_scheduler(const pool_lease_scheduler&) = delete;
    pool_lease_scheduler& operator=(const pool_lease_scheduler&) = delete;
    pool_lease_scheduler(pool_lease_scheduler&&) = delete;
    pool_lease_scheduler& operator=(pool_lease_scheduler&&) = delete;
    ~pool_lease_scheduler();

    [[nodiscard]] task<pool_waiter_result> acquire(
        const std::optional<std::chrono::milliseconds>& timeout);
    [[nodiscard]] task<pool_waiter_result> acquire(const std::optional<std::chrono::milliseconds>& timeout,
        stop_token stop_token);

    [[nodiscard]] pool_lease_release_status release(std::size_t slot) noexcept;
    [[nodiscard]] bool close() noexcept;
    void scan_deadlines(std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] bool closing() const noexcept;

private:
    class impl_type;
    std::unique_ptr<impl_type> impl_;
};

}  // namespace ruvia
