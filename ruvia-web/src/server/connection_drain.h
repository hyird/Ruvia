#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>

#include "ruvia/core/worker_timer.h"

namespace ruvia::detail {

// One worker's admitted-connection count and the graceful-drain barrier built
// on it. The worker opens the barrier when it starts serving and begins the
// drain when admission closes; the barrier completes when the last connection
// is released, or when the worker stops outright. The lifecycle thread waits
// for it with a deadline. Counting is lock-free; only the cold barrier
// transitions lock.
class connection_drain final {
public:
    connection_drain() = default;
    connection_drain(const connection_drain&) = delete;
    connection_drain& operator=(const connection_drain&) = delete;

    // Any thread: the relaxed gauge read by stats() and the acceptor.
    [[nodiscard]] std::size_t active() const noexcept {
        return active_.load(std::memory_order_relaxed);
    }

    // Worker thread: one admitted connection.
    void acquire() noexcept {
        active_.fetch_add(1, std::memory_order_relaxed);
    }

    // Worker thread: one closed connection. Completes a begun drain when it
    // was the last.
    void release() noexcept {
        const auto previous = active_.fetch_sub(1, std::memory_order_relaxed);
        if (previous == 0) {
            std::terminate();
        }
        if (previous == 1 && draining_) {
            complete();
        }
    }

    // Worker thread, once serving starts: a later wait blocks until the drain
    // completes.
    void open() noexcept {
        const std::lock_guard lock(mutex_);
        if (phase_ == phase_type::idle) {
            phase_ = phase_type::open;
        }
    }

    // Worker thread, when admission closes: completes now without connections.
    void begin() noexcept {
        draining_ = true;
        if (active() == 0) {
            complete();
        }
    }

    // Any thread: completes the barrier unconditionally, as a stopped worker
    // has nothing left to drain.
    void complete() noexcept {
        {
            const std::lock_guard lock(mutex_);
            phase_ = phase_type::completed;
        }
        changed_.notify_all();
    }

    // Lifecycle thread. Returns false when `deadline` passed first. A worker
    // that never started serving has nothing to drain.
    [[nodiscard]] bool wait_until(std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock(mutex_);
        while (phase_ == phase_type::open) {
            // Some standard libraries convert steady deadlines to system time.
            // Bound that conversion instead of passing a saturated deadline.
            const auto bounded_deadline = std::min(
                deadline, worker_timer_deadline_after(std::chrono::hours(1)));
            if (changed_.wait_until(lock, bounded_deadline, [this] { return phase_ != phase_type::open; })) {
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
        }
        return true;
    }

private:
    enum class phase_type : std::uint8_t {
        idle,
        open,
        completed,
    };

    std::atomic<std::size_t> active_{};
    // Worker-local.
    bool draining_{false};
    std::mutex mutex_;
    std::condition_variable changed_;
    phase_type phase_{phase_type::idle};
};

}  // namespace ruvia::detail
