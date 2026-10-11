#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <optional>
#include <variant>

#include "ruvia/core/pool_lease_release_status.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"

namespace ruvia {

namespace detail {
class pool_waiter;
}  // namespace detail

class pool_waiter_result;

class pool_waiter_acquired final {
public:
    [[nodiscard]] constexpr std::size_t index() const noexcept {
        return index_;
    }

private:
    friend class pool_waiter_result;

    explicit constexpr pool_waiter_acquired(std::size_t index) noexcept
        : index_(index) {}

    std::size_t index_;
};

class pool_waiter_timed_out final {
private:
    friend class pool_waiter_result;

    constexpr pool_waiter_timed_out() noexcept = default;
};

class pool_waiter_closed final {
private:
    friend class pool_waiter_result;

    constexpr pool_waiter_closed() noexcept = default;
};

class pool_waiter_cancelled final {
private:
    friend class pool_waiter_result;

    constexpr pool_waiter_cancelled() noexcept = default;
};

// Outcome of an asynchronous pool acquire. It owns exactly one alternative;
// only acquisition carries a slot index, so a failure never exposes a
// plausible sentinel. Only the scheduler can produce results.
class pool_waiter_result final {
public:
    [[nodiscard]] constexpr const pool_waiter_acquired* acquired() const& noexcept {
        return std::get_if<pool_waiter_acquired>(&value_);
    }
    [[nodiscard]] constexpr const pool_waiter_acquired* acquired() const&& = delete;

    [[nodiscard]] constexpr const pool_waiter_timed_out* timed_out() const& noexcept {
        return std::get_if<pool_waiter_timed_out>(&value_);
    }
    [[nodiscard]] constexpr const pool_waiter_timed_out* timed_out() const&& = delete;

    [[nodiscard]] constexpr const pool_waiter_closed* closed() const& noexcept {
        return std::get_if<pool_waiter_closed>(&value_);
    }
    [[nodiscard]] constexpr const pool_waiter_closed* closed() const&& = delete;

    [[nodiscard]] constexpr const pool_waiter_cancelled* cancelled() const& noexcept {
        return std::get_if<pool_waiter_cancelled>(&value_);
    }
    [[nodiscard]] constexpr const pool_waiter_cancelled* cancelled() const&& = delete;

private:
    friend class detail::pool_waiter;
    friend class pool_lease_scheduler;

    using value_type =
        std::variant<pool_waiter_acquired, pool_waiter_timed_out, pool_waiter_closed, pool_waiter_cancelled>;

    template <typename alternative_type>
    explicit constexpr pool_waiter_result(alternative_type alternative) noexcept
        : value_(alternative) {}

    [[nodiscard]] static constexpr pool_waiter_result make_acquired(std::size_t index) noexcept {
        return pool_waiter_result(pool_waiter_acquired(index));
    }
    [[nodiscard]] static constexpr pool_waiter_result make_timed_out() noexcept {
        return pool_waiter_result(pool_waiter_timed_out());
    }
    [[nodiscard]] static constexpr pool_waiter_result make_closed() noexcept {
        return pool_waiter_result(pool_waiter_closed());
    }
    [[nodiscard]] static constexpr pool_waiter_result make_cancelled() noexcept {
        return pool_waiter_result(pool_waiter_cancelled());
    }

    value_type value_;
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

    // A timeout arms a timer on the scheduler's worker. Once that worker's timer
    // queue stops (pending or new acquires alike) no timer fires: the acquire
    // completes only through release(), close() (closed), or its stop token
    // (cancelled). It never throws for that.
    [[nodiscard]] task<pool_waiter_result> acquire(
        const std::optional<std::chrono::milliseconds>& timeout);
    [[nodiscard]] task<pool_waiter_result> acquire(const std::optional<std::chrono::milliseconds>& timeout,
        stop_token stop_token);

    [[nodiscard]] pool_lease_release_status release(std::size_t slot) noexcept;
    [[nodiscard]] bool close() noexcept;
    [[nodiscard]] bool closing() const noexcept;

private:
    class impl_type;
    std::unique_ptr<impl_type> impl_;
};

}  // namespace ruvia
