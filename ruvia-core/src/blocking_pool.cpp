#include "ruvia/core/blocking_pool.h"

#include <algorithm>
#include <condition_variable>
#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ruvia/core/detail/util/failure_report.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/memory/process_resource.h"

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
    std::size_t requested, std::size_t thread_count) noexcept {
    if (requested != 0) {
        return requested;
    }
    return thread_count * 64;
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

blocking_operation_rejected::blocking_operation_rejected(blocking_status status)
    : std::runtime_error(std::string(describe_blocking_status(status))),
      status_(status) {
    if (status == blocking_status::completed) {
        throw std::invalid_argument("completed blocking operation was not rejected");
    }
}

struct blocking_pool::thread_state_type final {
    explicit thread_state_type()
        : threads_(detail::process_resource()) {}

    std::pmr::vector<std::thread> threads_;
};

struct blocking_pool::impl_type final {
    struct node_type final {
        explicit node_type(move_only_function<void()>&& value)
            : task_(std::move(value)) {}

        move_only_function<void()> task_;
        node_type* next_{nullptr};
    };

    explicit impl_type(const blocking_pool_options& options)
        : thread_count_(resolve_thread_count(options.thread_count_)),
          queue_capacity_(resolve_queue_capacity(options.queue_capacity_, thread_count_)),
          node_resource_(detail::process_resource()) {}

    void start(const std::shared_ptr<impl_type>& self, std::pmr::vector<std::thread>& threads) {
        threads.reserve(thread_count_);
        try {
            for (std::size_t i = 0; i < thread_count_; ++i) {
                threads.emplace_back([self] { self->run(); });
            }
        } catch (...) {
            // A pool that could not start all of its threads is not a pool the
            // caller asked for. Unwind the ones that did start before the
            // exception leaves construction.
            stop();
            join(threads);
            throw;
        }
    }

    void run() noexcept {
        for (;;) {
            std::unique_ptr<node_type, detail::pmr_object_deleter<node_type>> node_value(nullptr,
                detail::pmr_object_deleter<node_type>{node_resource_});
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this] { return stopping_ || queued_ != 0; });
                if (stopping_ || queued_ == 0) {
                    // Stopping discards every task that has not already been
                    // picked up. A worker must never start queued work after
                    // the stop request becomes visible.
                    return;
                }
                node_value.reset(queue_head_);
                queue_head_ = node_value->next_;
                node_value->next_ = nullptr;
                if (queue_head_ == nullptr) {
                    queue_tail_ = nullptr;
                }
                --queued_;
                ++running_;
            }
            try {
                node_value->task_();
            } catch (...) {
                // The blocking wrapper catches the callable's exceptions and
                // hands them back to the waiter, so reaching this is a raw
                // submit() whose task threw with nobody to receive it.
                detail::report_unhandled_failure("blocking pool task", std::current_exception());
            }
            // The task owns the completion it answered; destroy it here rather
            // than under the lock the next iteration takes.
            node_value.reset();
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
        node_type* dropped = nullptr;
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                return;
            }
            stopping_ = true;
            discarded_ += queued_;
            dropped = queue_head_;
            queue_head_ = nullptr;
            queue_tail_ = nullptr;
            queued_ = 0;
        }
        condition_.notify_all();

        // The detached chain owns all queued callables. Destroy nodes
        // iteratively outside the mutex so callable destructors may reenter
        // stats(), stop(), or submit() safely without allocation or recursion.
        std::unique_ptr<node_type, detail::pmr_object_deleter<node_type>> node_value(
            dropped, detail::pmr_object_deleter<node_type>{node_resource_});
        while (node_value != nullptr) {
            node_type* next_value = node_value->next_;
            node_value->next_ = nullptr;
            node_value.reset(next_value);
        }
    }

    static void throw_if_current(const std::pmr::vector<std::thread>& threads) {
        if (std::ranges::any_of(threads, [](const std::thread& thread) {
                return thread.joinable() && thread.get_id() == std::this_thread::get_id();
            })) {
            throw std::logic_error("cannot join a blocking pool from one of its threads");
        }
    }

    static void join(std::pmr::vector<std::thread>& threads) {
        throw_if_current(threads);
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    static void detach(std::pmr::vector<std::thread>& threads) noexcept {
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.detach();
            }
        }
    }

    std::size_t thread_count_;
    std::size_t queue_capacity_;
    std::pmr::memory_resource* node_resource_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    node_type* queue_head_{nullptr};
    node_type* queue_tail_{nullptr};
    std::size_t queued_{0};
    std::size_t running_{0};
    std::uint64_t completed_{0};
    std::uint64_t rejected_{0};
    std::uint64_t discarded_{0};
    bool stopping_{false};
};

blocking_pool::blocking_pool(blocking_pool_options options)
    : impl_(std::make_shared<impl_type>(options)),
      threads_(std::make_unique<thread_state_type>()) {
    impl_->start(impl_, threads_->threads_);
}

blocking_pool::~blocking_pool() {
    stop();
    impl_type::detach(threads_->threads_);
}

std::size_t blocking_pool::thread_count() const noexcept {
    return threads_->threads_.size();
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
    {
        std::lock_guard lock(impl_->mutex_);
        if (impl_->stopping_) {
            ++impl_->discarded_;
            return blocking_submit_status::pool_stopped;
        }
        if (impl_->queued_ >= impl_->queue_capacity_) {
            ++impl_->rejected_;
            return blocking_submit_status::queue_full;
        }
    }
    auto node_value = detail::make_pmr_object<impl_type::node_type>(impl_->node_resource_, std::move(task_value));
    blocking_submit_status status = blocking_submit_status::accepted;
    {
        std::lock_guard lock(impl_->mutex_);
        if (impl_->stopping_) {
            ++impl_->discarded_;
            status = blocking_submit_status::pool_stopped;
        } else if (impl_->queued_ >= impl_->queue_capacity_) {
            ++impl_->rejected_;
            status = blocking_submit_status::queue_full;
        } else {
            if (impl_->queue_tail_ != nullptr) {
                impl_->queue_tail_->next_ = node_value.get();
            } else {
                impl_->queue_head_ = node_value.get();
            }
            impl_->queue_tail_ = node_value.release();
            ++impl_->queued_;
        }
    }
    if (status != blocking_submit_status::accepted) {
        return status;
    }
    impl_->condition_.notify_one();
    return blocking_submit_status::accepted;
}

void blocking_pool::stop() noexcept {
    impl_->stop();
}

void blocking_pool::join() {
    impl_type::throw_if_current(threads_->threads_);
    stop();
    impl_type::join(threads_->threads_);
}

}  // namespace ruvia
