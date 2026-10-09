#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/detail/worker/worker_dispatcher.h"
#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/core/mpsc_ring_queue.h"

// The dispatcher's state, declared here because two translation units own parts
// of it: worker_dispatcher.cpp runs the queue and the worker's lifecycle, while
// worker_timers.cpp runs the timer heap that shares its mutex.

namespace ruvia::detail {

inline constexpr std::size_t no_timer_slot = static_cast<std::size_t>(-1);

// One pending deadline in the heap. `slot` and `generation` identify the
// registration it belongs to, so a cancelled entry is recognised as stale
// instead of being searched for and removed.
struct timer_entry final {
    std::chrono::steady_clock::time_point deadline_;
    std::uint64_t sequence_{0};
    std::size_t slot_{no_timer_slot};
    std::uint64_t generation_{0};
};

// A registration slot, reused through a free list; `generation` invalidates every
// heap entry that referred to a previous occupant.
struct timer_slot final {
    std::uint64_t generation_{0};
    bool active_{false};
    std::size_t next_free_{no_timer_slot};
    move_only_function<void(worker_timer_outcome)> completion_;
};

// Heap order: earliest deadline first, ties broken by registration order.
struct timer_entry_later final {
    bool operator()(const timer_entry& left, const timer_entry& right) const noexcept {
        return left.deadline_ > right.deadline_ ||
               (left.deadline_ == right.deadline_ && left.sequence_ > right.sequence_);
    }
};

// The worker whose run loop is executing on this thread, or nullptr. A plain
// thread-local read: the timer path uses it to take the on-worker shortcut
// before it would otherwise lock.
[[nodiscard]] const worker_dispatcher* current_worker_dispatcher() noexcept;

// The process-wide worker id source.
[[nodiscard]] worker_id_type next_worker_dispatcher_id() noexcept;

struct worker_dispatcher::impl_type {
    static std::size_t checked_queue_capacity(std::size_t capacity) {
        if (capacity == 0 || capacity == std::numeric_limits<std::size_t>::max()) {
            throw std::invalid_argument("worker queue capacity is out of range");
        }
        return capacity;
    }
    explicit impl_type(asio::io_context& context_value, std::size_t requested_capacity)
        : io_context_(context_value),
          timer_(std::make_unique<asio::steady_timer>(context_value)),
          nodes_(detail::process_resource()),
          ready_queue_(checked_queue_capacity(requested_capacity), detail::process_resource()),
          mutex_(ready_queue_.synchronization_mutex()),
          worker_id_(next_worker_dispatcher_id()),
          timers_(detail::process_resource()),
          timer_slots_(detail::process_resource()) {
        nodes_.resize(requested_capacity + 1);
        for (std::size_t index = 0; index + 1 < nodes_.size(); ++index) {
            nodes_[index].next_ = index + 1;
        }
        free_head_ = 0;
        timers_.reserve(requested_capacity);
        timer_slots_.reserve(requested_capacity);
    }

    asio::io_context& io_context_;
    std::unique_ptr<asio::steady_timer> timer_;
    enum class node_state_type : std::uint8_t { free,
        reserved,
        ready,
        active,
        releasing };
    struct node_type final {
        move_only_function<void()> task_;
        std::size_t next_{no_timer_slot};
        node_state_type state_{node_state_type::free};
    };
    std::pmr::vector<node_type> nodes_;
    mpsc_ring_queue<std::size_t> ready_queue_;
    std::mutex& mutex_;
    std::condition_variable pending_changed_;
    std::size_t free_head_{no_timer_slot};
    std::size_t pending_count_{0};
    std::size_t active_count_{0};
    idle_callbacks_type idle_waiters_{detail::process_resource()};
    worker_id_type worker_id_{0};
    std::atomic_bool accepting_{true};
    std::atomic_bool context_attached_{true};
    bool drain_scheduled_{false};
    bool abandon_drain_{false};
    bool shutdown_notification_active_{false};
    idle_callbacks_type shutdown_notification_waiters_{detail::process_resource()};
    shutdown_listeners_type shutdown_listeners_{detail::process_resource()};
    std::pmr::vector<timer_entry> timers_;
    std::pmr::vector<timer_slot> timer_slots_;
    std::size_t free_timer_slot_{no_timer_slot};
    std::uint64_t next_timer_sequence_{0};
    std::uint64_t timer_generation_{0};
    bool timer_armed_{false};
    bool dispatching_timers_{false};
    std::atomic_bool timers_stopping_{false};
    std::size_t stale_timer_count_{0};
};

}  // namespace ruvia::detail
