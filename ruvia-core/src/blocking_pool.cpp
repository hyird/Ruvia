#include "ruvia/core/blocking_pool.h"

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ruvia/core/failure_report.h"
#include "ruvia/core/local_ring_queue.h"
#include "ruvia/core/memory/pmr_object.h"

namespace ruvia {

namespace {

[[nodiscard]] std::size_t resolve_thread_count(std::size_t requested) noexcept {
    if (requested != 0) {
        return requested;
    }
    const auto hardware_threads = std::size_t{std::thread::hardware_concurrency()};
    const auto half_hardware_threads = (hardware_threads + 1) / 2;
    return std::clamp(half_hardware_threads, std::size_t{2}, std::size_t{8});
}

[[nodiscard]] std::size_t resolve_queue_capacity(
    std::size_t requested, std::size_t thread_count) {
    constexpr std::size_t default_per_thread_capacity = 64;
    if (requested == 0 &&
        thread_count > std::numeric_limits<std::size_t>::max() / default_per_thread_capacity) {
        throw std::invalid_argument("blocking pool storage capacity is out of range");
    }
    const auto capacity = requested != 0 ? requested : thread_count * default_per_thread_capacity;
    if (thread_count > std::numeric_limits<std::size_t>::max() - capacity) {
        throw std::invalid_argument("blocking pool storage capacity is out of range");
    }
    return capacity;
}

}  // namespace

std::string_view describe_blocking_status(blocking_status status) noexcept {
    switch (status) {
        case blocking_status::completed:
            return "completed";
        case blocking_status::queue_full:
            return "blocking pool queue is full";
        case blocking_status::pool_stopped:
            return "blocking pool is stopped";
        case blocking_status::worker_stopping:
            return "worker is stopping";
        case blocking_status::cancelled:
            return "blocking operation was cancelled";
        case blocking_status::timed_out:
            return "blocking operation timed out";
    }
    return "unknown";
}

// Only blocking_result::value() constructs this, after ruling out completed.
blocking_operation_rejected::blocking_operation_rejected(blocking_status status)
    : std::runtime_error(std::string(describe_blocking_status(status))),
      status_(status) {}

struct blocking_pool::impl_type final {
    struct node_type final {
        move_only_function<void()> task_;
        node_type* next_{nullptr};
    };

    explicit impl_type(const blocking_pool_options& options)
        : thread_count_(resolve_thread_count(options.thread_count_)),
          queue_capacity_(resolve_queue_capacity(options.queue_capacity_, thread_count_)),
          queue_(queue_capacity_, process_resource()),
          nodes_(process_resource()),
          threads_(process_resource()) {
        // Reserved/queued submissions occupy at most queue_capacity_ nodes;
        // each thread may also hold one node while moving its callable out.
        nodes_.resize(queue_capacity_ + thread_count_);
        for (auto& node : nodes_) {
            node.next_ = free_head_;
            free_head_ = &node;
        }
    }

    void release_node(node_type& node) noexcept {
        std::lock_guard lock(mutex_);
        node.next_ = free_head_;
        free_head_ = &node;
    }

    void start(const std::shared_ptr<impl_type>& self) {
        threads_.reserve(thread_count_);
        try {
            for (std::size_t i = 0; i < thread_count_; ++i) {
                threads_.emplace_back([self] { self->run(); });
            }
        } catch (...) {
            // A pool that could not start all of its threads is not a pool the
            // caller asked for. Unwind the ones that did start before the
            // exception leaves construction.
            stop();
            join_threads();
            throw;
        }
    }

    void run() noexcept {
        for (;;) {
            move_only_function<void()> task;
            node_type* node = nullptr;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this] { return stopping_ || queued_ != 0; });
                if (stopping_) {
                    // Stopping discards every task that has not already been
                    // picked up. A worker must never start queued work after
                    // the stop request becomes visible.
                    return;
                }
                if (!queue_.try_pop(node)) {
                    std::terminate();
                }
                --queued_;
                ++running_;
            }
            task = std::move(node->task_);
            release_node(*node);
            try {
                task();
            } catch (...) {
                // The blocking wrapper catches the callable's exceptions and
                // hands them back to the waiter, so reaching this is a raw
                // submit() whose task threw with nobody to receive it.
                report_unhandled_failure("blocking pool task", std::current_exception());
            }
            // The task owns the completion it answered; destroy it here rather
            // than under the lock the next iteration takes.
            task = nullptr;
            {
                std::lock_guard lock(mutex_);
                --running_;
                ++completed_;
            }
        }
    }

    void stop() noexcept {
        // Only the first stop caller owns the drain. In particular, a queued
        // task's destructor may call stop() again; letting that call recurse
        // into the drain would grow the stack once per queued task.
        std::size_t dropped = 0;
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                return;
            }
            stopping_ = true;
            discarded_ += queued_;
            dropped = std::exchange(queued_, 0);
        }
        condition_.notify_all();

        // Once stopping is published no thread pops or pushes the ring again,
        // so the first stop caller owns the remaining slots. Destroy them
        // outside the mutex so callable destructors may reenter stats(),
        // stop(), or submit() safely without recursion.
        node_type* node = nullptr;
        while (dropped-- != 0) {
            if (!queue_.try_pop(node)) {
                std::terminate();
            }
            node->task_ = nullptr;
            release_node(*node);
        }
    }

    void throw_if_current() const {
        if (std::ranges::any_of(threads_, [](const std::thread& thread) {
                return thread.joinable() && thread.get_id() == std::this_thread::get_id();
            })) {
            throw std::logic_error("cannot join a blocking pool from one of its threads");
        }
    }

    void join_threads() {
        for (auto& thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    void detach_threads() noexcept {
        for (auto& thread : threads_) {
            if (thread.joinable()) {
                thread.detach();
            }
        }
    }

    std::size_t thread_count_;
    std::size_t queue_capacity_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    // Queue operations move only pointers under mutex_. A claimed node stays
    // exclusive while user callable moves/destruction run outside the lock.
    local_ring_queue<node_type*> queue_;
    std::pmr::vector<node_type> nodes_;
    std::pmr::vector<std::thread> threads_;
    node_type* free_head_{nullptr};
    std::size_t queued_{0};
    std::size_t reserved_{0};
    std::size_t running_{0};
    std::uint64_t completed_{0};
    std::uint64_t rejected_{0};
    std::uint64_t discarded_{0};
    bool stopping_{false};
};

blocking_pool::blocking_pool(blocking_pool_options options)
    : impl_(std::allocate_shared<impl_type>(
          std::pmr::polymorphic_allocator<impl_type>(process_resource()), options)) {
    impl_->start(impl_);
}

blocking_pool::~blocking_pool() {
    stop();
    impl_->detach_threads();
}

std::size_t blocking_pool::thread_count() const noexcept {
    return impl_->thread_count_;
}

std::size_t blocking_pool::queue_capacity() const noexcept {
    return impl_->queue_capacity_;
}

blocking_pool_stats blocking_pool::stats() const noexcept {
    std::lock_guard lock(impl_->mutex_);
    return blocking_pool_stats{
        .queued_ = impl_->queued_,
        .running_ = impl_->running_,
        .completed_ = impl_->completed_,
        .rejected_ = impl_->rejected_,
        .discarded_ = impl_->discarded_,
    };
}

blocking_submit_status blocking_pool::submit(move_only_function<void()> task_value) {
    if (!task_value) {
        throw std::invalid_argument("blocking pool requires a callable task");
    }
    const auto owner_ptr = impl_;
    auto& owner = *owner_ptr;
    impl_type::node_type* node = nullptr;
    {
        std::lock_guard lock(owner.mutex_);
        if (owner.stopping_) {
            ++owner.discarded_;
            return blocking_submit_status::pool_stopped;
        }
        if (owner.queued_ + owner.reserved_ >= owner.queue_capacity_) {
            ++owner.rejected_;
            return blocking_submit_status::queue_full;
        }
        node = owner.free_head_;
        owner.free_head_ = node->next_;
        ++owner.reserved_;
    }
    node->task_ = std::move(task_value);
    bool stopped = false;
    {
        std::lock_guard lock(owner.mutex_);
        --owner.reserved_;
        stopped = owner.stopping_;
        if (stopped) {
            ++owner.discarded_;
        } else {
            if (!owner.queue_.try_push(node)) {
                std::terminate();
            }
            ++owner.queued_;
        }
    }
    if (stopped) {
        node->task_ = nullptr;
        owner.release_node(*node);
        return blocking_submit_status::pool_stopped;
    }
    owner.condition_.notify_one();
    return blocking_submit_status::accepted;
}

void blocking_pool::stop() noexcept {
    const auto owner = impl_;
    owner->stop();
}

void blocking_pool::join() {
    impl_->throw_if_current();
    stop();
    impl_->join_threads();
}

}  // namespace ruvia
