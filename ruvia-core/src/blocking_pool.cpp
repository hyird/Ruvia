#include "blocking_pool.h"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "failure_report.h"
#include "move_only_function.h"
#include "runtime_thread.h"

namespace ruvia {

namespace {

// Bounded single-thread pointer FIFO ring allocated once at pool startup.
template <typename pointer_type>
class local_ring_queue final {
public:
    explicit local_ring_queue(std::size_t capacity) :
        slots_(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("ring queue capacity must be nonzero");
        }
    }

    local_ring_queue(const local_ring_queue&) = delete;
    local_ring_queue& operator=(const local_ring_queue&) = delete;
    local_ring_queue(local_ring_queue&&) = delete;
    local_ring_queue& operator=(local_ring_queue&&) = delete;

    [[nodiscard]] bool try_push(pointer_type value) noexcept {
        if (size_ == slots_.size()) {
            return false;
        }
        const auto remaining = slots_.size() - head_;
        const auto index = size_ >= remaining ? size_ - remaining : head_ + size_;
        slots_[index] = value;
        ++size_;
        return true;
    }

    [[nodiscard]] bool try_pop(pointer_type& value) noexcept {
        if (size_ == 0) {
            return false;
        }
        value = slots_[head_];
        head_ = head_ + 1 == slots_.size() ? 0 : head_ + 1;
        --size_;
        return true;
    }

private:
    std::vector<pointer_type> slots_;
    std::size_t head_{0};
    std::size_t size_{0};
};

[[nodiscard]] std::size_t resolve_thread_count(std::size_t requested) noexcept {
    if (requested != 0) {
        return requested;
    }
    const auto hardware_threads = std::size_t{std::thread::hardware_concurrency()};
    const auto half_hardware_threads = (hardware_threads + 1) / 2;
    return std::clamp(half_hardware_threads, std::size_t{2}, std::size_t{8});
}

[[nodiscard]] std::size_t resolve_queue_capacity(std::size_t requested, std::size_t thread_count) {
    constexpr std::size_t default_per_thread_capacity = 64;
    if (requested == 0 && thread_count > std::numeric_limits<std::size_t>::max() / default_per_thread_capacity) {
        throw std::invalid_argument("blocking pool storage capacity is out of range");
    }
    const auto capacity = requested != 0 ? requested : thread_count * default_per_thread_capacity;
    if (thread_count > std::numeric_limits<std::size_t>::max() - capacity) {
        throw std::invalid_argument("blocking pool storage capacity is out of range");
    }
    return capacity;
}

thread_local const void* g_current_blocking_pool = nullptr;
}  // namespace

std::string_view describe_blocking_status(blocking_status status) noexcept {
    switch (status) {
        case blocking_status::completed:
            return "completed";
        case blocking_status::failed:
            return "blocking operation failed";
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
        case blocking_status::unavailable:
            return "worker has no blocking pool";
    }
    return "unknown";
}

// Only blocking_result::value() constructs this, after ruling out completed.
blocking_operation_rejected::blocking_operation_rejected(blocking_status status) :
    std::runtime_error(std::string(describe_blocking_status(status))),
    status_(status) {}

struct blocking_pool::impl_type final {
    struct node_type final {
        move_only_function<void()> task_;
        node_type* next_{nullptr};
    };

    // A callable may reenter join() while the current stack still owns an
    // admission slot. Nested posts to other pools must not hide that slot.
    class reserved_post_scope final {
    public:
        explicit reserved_post_scope(const impl_type& pool) noexcept :
            pool_(&pool),
            previous_(std::exchange(current_, this)) {}
        reserved_post_scope(const reserved_post_scope&) = delete;
        reserved_post_scope& operator=(const reserved_post_scope&) = delete;
        ~reserved_post_scope() {
            current_ = previous_;
        }

        [[nodiscard]] static bool contains(const impl_type& pool) noexcept {
            for (auto* scope = current_; scope != nullptr; scope = scope->previous_) {
                if (scope->pool_ == &pool) {
                    return true;
                }
            }
            return false;
        }

    private:
        static inline thread_local const reserved_post_scope* current_ = nullptr;
        const impl_type* pool_;
        const reserved_post_scope* previous_;
    };

    explicit impl_type(const blocking_pool_options& options) :
        thread_count_(resolve_thread_count(options.thread_count_)),
        queue_capacity_(resolve_queue_capacity(options.queue_capacity_, thread_count_)),
        queue_(queue_capacity_),
        nodes_(),
        threads_() {
        // Reserved/queued posts occupy at most queue_capacity_ nodes;
        // each thread may also hold one node while moving its callable out.
        nodes_.resize(queue_capacity_ + thread_count_);
        busy_.resize(thread_count_);
        busy_at_retirement_.resize(thread_count_);
        for (auto& node : nodes_) {
            node.next_ = free_head_;
            free_head_ = &node;
        }
    }

    void start(const std::shared_ptr<impl_type>& self) {
        threads_.reserve(thread_count_);
        try {
            for (std::size_t i = 0; i < thread_count_; ++i) {
                threads_.emplace_back([owner = self, i]() noexcept {
                    owner->run(i);
                });
            }
        } catch (...) {
            // A pool that could not start all of its threads is not a pool the
            // caller asked for. Unwind the ones that did start before the
            // exception leaves construction.
            request_stop();
            join_threads();
            throw;
        }
    }

    void run(std::size_t index) noexcept {
        struct current_pool_guard final {
            const void* previous_;
            explicit current_pool_guard(const impl_type* pool) noexcept :
                previous_(std::exchange(g_current_blocking_pool, pool)) {}
            ~current_pool_guard() {
                g_current_blocking_pool = previous_;
            }
        } current_guard(this);
        std::unique_lock lock(mutex_);
        for (;;) {
            condition_.wait(lock, [this] {
                return stopping_ || queued_ != 0;
            });
            if (stopping_) {
                // Stopping discards every task that has not already been
                // picked up. A worker must never start queued work after
                // the stop request becomes visible.
                return;
            }
            node_type* node = nullptr;
            if (!queue_.try_pop(node)) {
                std::terminate();
            }
            --queued_;
            ++running_;
            busy_[index] = 1;
            lock.unlock();

            try {
                node->task_();
            } catch (...) {
                // The blocking wrapper catches the callable's exceptions and
                // hands them back to the waiter, so reaching this is a raw
                // post() whose task threw with nobody to receive it.
                report_unhandled_failure("blocking pool task", std::current_exception());
            }
            // The claimed node stays exclusive until recycling. Retire its
            // completion outside the lock without first relocating the task.
            node->task_ = nullptr;

            lock.lock();
            node->next_ = free_head_;
            free_head_ = node;
            --running_;
            busy_[index] = 0;
            ++completed_;
        }
    }

    void request_stop() noexcept {
        // Only the first stop caller owns the drain. In particular, a queued
        // task's destructor may call request_stop() again; letting that call recurse
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
            if (dropped != 0) {
                stop_drain_thread_ = std::this_thread::get_id();
            }
        }
        condition_.notify_all();
        if (dropped == 0) {
            return;
        }

        // Once stopping is published no thread pops or pushes the ring again,
        // so the first stop caller owns the remaining slots. Destroy them
        // outside the mutex so callable destructors may reenter stats(),
        // request_stop(), or post() safely without recursion.
        // Stopping permanently closes admission, so drained nodes need no recycling.
        node_type* node = nullptr;
        while (dropped-- != 0) {
            if (!queue_.try_pop(node)) {
                std::terminate();
            }
            node->task_ = nullptr;
        }
        {
            std::lock_guard lock(mutex_);
            stop_drain_thread_ = {};
        }
        condition_.notify_all();
    }

    [[nodiscard]] bool is_current_thread() const noexcept {
        return g_current_blocking_pool == this;
    }

    void join_threads() {
        for (auto& thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    void join() {
        if (is_current_thread()) {
            throw std::logic_error("cannot join a blocking pool from one of its threads");
        }
        if (reserved_post_scope::contains(*this)) {
            throw std::logic_error("cannot join a blocking pool while posting to it");
        }
        request_stop();
        std::vector<detail::runtime_thread> joining_threads;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] {
                return !joining_;
            });
            if (!joined_) {
                joining_ = true;
                joining_threads.swap(threads_);
            }
        }
        for (auto& thread : joining_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        {
            std::unique_lock lock(mutex_);
            if (joining_) {
                joining_ = false;
                joined_ = true;
                condition_.notify_all();
            }
            condition_.wait(lock, [this] {
                return (stop_drain_thread_ == std::thread::id{} || stop_drain_thread_ == std::this_thread::get_id()) && reserved_ == 0;
            });
        }
    }

    // After the stop is published an idle thread can only leave its loop, so it
    // is joined and its thread-local state is gone when the pool is. A thread
    // inside a callable cannot be interrupted and must not delay destruction:
    // it is detached and keeps this state alive through its own reference.
    void retire_threads() noexcept {
        std::vector<detail::runtime_thread> retiring_threads;
        {
            std::lock_guard lock(mutex_);
            retiring_threads.swap(threads_);
            busy_at_retirement_ = busy_;
        }
        for (std::size_t index = 0; index < retiring_threads.size(); ++index) {
            auto& thread = retiring_threads[index];
            if (!thread.joinable()) {
                continue;
            }
            if (busy_at_retirement_[index] != 0) {
                thread.detach();
            } else {
                thread.join();
            }
        }
    }

    template <typename prepare_type, typename settle_type>
    auto post(prepare_type prepare, settle_type settle);

    void release_reservation(node_type& node) noexcept {
        bool notify = false;
        {
            std::lock_guard lock(mutex_);
            --reserved_;
            if (stopping_) {
                notify = reserved_ == 0;
            } else {
                node.next_ = free_head_;
                free_head_ = &node;
            }
        }
        if (notify) {
            condition_.notify_all();
        }
    }

    std::size_t thread_count_;
    std::size_t queue_capacity_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    // Queue operations move only pointers under mutex_. A claimed node stays
    // exclusive while user callable moves/destruction run outside the lock.
    local_ring_queue<node_type*> queue_;
    std::vector<node_type> nodes_;
    std::vector<detail::runtime_thread> threads_;
    // Indexed like threads_; set while that thread holds a claimed callable.
    std::vector<unsigned char> busy_;
    // Same capacity as busy_, so the destructor's snapshot never allocates.
    std::vector<unsigned char> busy_at_retirement_;
    node_type* free_head_{nullptr};
    std::size_t queued_{0};
    std::size_t reserved_{0};
    std::size_t running_{0};
    std::uint64_t completed_{0};
    std::uint64_t rejected_{0};
    std::uint64_t discarded_{0};
    bool stopping_{false};
    // A nonempty ID owns the stop drain, including destructor reentry.
    std::thread::id stop_drain_thread_{};
    bool joining_{false};
    bool joined_{false};
};

blocking_pool::blocking_pool(blocking_pool_options options) :
    impl_(std::make_shared<impl_type>(options)) {
    impl_->start(impl_);
}

blocking_pool::~blocking_pool() {
    request_stop();
    impl_->retire_threads();
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

template <typename prepare_type, typename settle_type>
auto blocking_pool::impl_type::post(prepare_type prepare, settle_type settle) {
    node_type* node = nullptr;
    std::optional<post_status> refused;
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            ++discarded_;
            refused = post_status::stopping;
        } else if (queued_ + reserved_ >= queue_capacity_) {
            ++rejected_;
            refused = post_status::queue_full;
        } else {
            node = free_head_;
            free_head_ = node->next_;
            ++reserved_;
        }
    }
    if (refused) {
        return settle(*refused, nullptr);
    }
    try {
        const reserved_post_scope reservation(*this);
        prepare(node->task_);
    } catch (...) {
        // Preparation can throw before it stores a callable in the node.
        release_reservation(*node);
        throw;
    }
    bool stopped = false;
    {
        std::lock_guard lock(mutex_);
        stopped = stopping_;
        if (stopped) {
            ++discarded_;
        } else {
            --reserved_;
            if (!queue_.try_push(node)) {
                std::terminate();
            }
            ++queued_;
        }
    }
    if (stopped) {
        // Keep the reservation visible to join() until the rejected callable
        // has been returned or destroyed, without holding the queue mutex.
        auto rejected = [&] {
            const reserved_post_scope reservation(*this);
            return settle(post_status::stopping, &node->task_);
        }();
        release_reservation(*node);
        return rejected;
    }
    condition_.notify_one();
    return settle(post_status::accepted, nullptr);
}

post_result<> blocking_pool::post(move_only_function<void()> task_value) {
    if (!task_value) {
        throw std::invalid_argument("blocking pool requires a callable task");
    }
    const auto owner = impl_;
    return owner->post([&task_value](move_only_function<void()>& slot) noexcept {
        slot = std::move(task_value);
    }, [&task_value](post_status status, move_only_function<void()>* rejected) {
        if (status == post_status::accepted) {
            return detail::delivery_access::accept<post_result<>>();
        }
        return detail::delivery_access::reject<post_result<>>(status, std::move(rejected == nullptr ? task_value : *rejected));
    });
}

post_status detail::blocking_pool_access::post_factory(blocking_pool& pool, move_only_function<move_only_function<void()>()> factory) {
    const auto owner = pool.impl_;
    return owner->post([&factory](move_only_function<void()>& slot) {
        slot = factory();
    }, [](post_status status, move_only_function<void()>* rejected) noexcept {
        if (rejected != nullptr) {
            *rejected = nullptr;
        }
        return status;
    });
}

void blocking_pool::request_stop() noexcept {
    const auto owner = impl_;
    owner->request_stop();
}

void blocking_pool::join() {
    const auto owner = impl_;
    owner->join();
}

}  // namespace ruvia
