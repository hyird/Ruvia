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

namespace ruvia {

// Bounded single-thread FIFO ring. All slots are constructed at startup and stay
// constructed; no queue operation allocates afterwards. Payload assignment may
// allocate or throw independently of the queue. Callers that share it across
// threads must serialize every operation themselves.
//
// prepare_push reserves one unpublished slot: it cannot be prepared again until
// commit_push publishes it or cancel_push releases it. Direct prepared-slot
// assignment failure requires cancel before re-preparing; try_push cancels its
// hidden reservation before rethrowing. A front() borrow stays valid until pop.
template <typename value_type>
class local_ring_queue final {
public:
    explicit local_ring_queue(std::size_t capacity) :
        storage_(allocate_storage(capacity)) {}

    local_ring_queue(const local_ring_queue&) = delete;
    local_ring_queue& operator=(const local_ring_queue&) = delete;
    local_ring_queue(local_ring_queue&&) = delete;
    local_ring_queue& operator=(local_ring_queue&&) = delete;
    ~local_ring_queue() = default;

    [[nodiscard]] std::size_t capacity() const noexcept {
        return storage_.get_deleter().capacity_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }

    [[nodiscard]] bool full() const noexcept {
        return size_ == capacity();
    }

    [[nodiscard]] value_type* prepare_push() & noexcept {
        if (prepared_) {
            std::terminate();
        }
        if (size_ == capacity()) {
            return nullptr;
        }
        prepared_ = true;
        return storage_.get() + slot_index(size_);
    }
    value_type* prepare_push() && = delete;

    void commit_push() noexcept {
        if (!prepared_) {
            std::terminate();
        }
        prepared_ = false;
        ++size_;
    }

    void cancel_push() noexcept {
        prepared_ = false;
    }

    [[nodiscard]] bool try_push(const value_type& value) noexcept(std::is_nothrow_copy_assignable_v<value_type>) {
        return push_value(value);
    }

    [[nodiscard]] bool try_push(value_type&& value) noexcept(std::is_nothrow_move_assignable_v<value_type>) {
        return push_value(std::move(value));
    }

    [[nodiscard]] value_type* front() & noexcept {
        return size_ == 0 ? nullptr : storage_.get() + head_;
    }
    [[nodiscard]] const value_type* front() const& noexcept {
        return size_ == 0 ? nullptr : storage_.get() + head_;
    }
    value_type* front() && = delete;
    const value_type* front() const&& = delete;

    void pop() noexcept {
        if (size_ == 0) {
            std::terminate();
        }
        head_ = slot_index(1);
        --size_;
    }

    [[nodiscard]] bool try_pop(value_type& value) noexcept(std::is_nothrow_move_assignable_v<value_type>) {
        auto* slot = front();
        if (!slot) {
            return false;
        }
        value = std::move(*slot);
        pop();
        return true;
    }

private:
    struct storage_deleter final {
        std::size_t capacity_{0};
        std::size_t constructed_{0};

        void operator()(value_type* storage) const noexcept {
            std::destroy_n(storage, constructed_);
            std::allocator<value_type>{}.deallocate(storage, capacity_);
        }
    };

    using storage_owner = std::unique_ptr<value_type, storage_deleter>;

    [[nodiscard]] static storage_owner allocate_storage(std::size_t capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("ring queue capacity must be nonzero");
        }
        std::allocator<value_type> allocator;
        storage_owner storage(allocator.allocate(capacity), storage_deleter{capacity, 0});
        auto& constructed = storage.get_deleter().constructed_;
        while (constructed != capacity) {
            std::construct_at(storage.get() + constructed);
            ++constructed;
        }
        return storage;
    }

    // Physical index of the slot `offset` positions after the head; offset <= capacity.
    [[nodiscard]] std::size_t slot_index(std::size_t offset) const noexcept {
        const auto remaining = capacity() - head_;
        return offset >= remaining ? offset - remaining : head_ + offset;
    }

    template <typename input_type>
    [[nodiscard]] bool push_value(input_type&& value) noexcept(std::is_nothrow_assignable_v<value_type&, input_type&&>) {
        auto* slot = prepare_push();
        if (!slot) {
            return false;
        }
        if constexpr (std::is_nothrow_assignable_v<value_type&, input_type&&>) {
            *slot = std::forward<input_type>(value);
        } else {
            try {
                *slot = std::forward<input_type>(value);
            } catch (...) {
                cancel_push();
                throw;
            }
        }
        commit_push();
        return true;
    }

    storage_owner storage_;
    std::size_t head_{0};
    std::size_t size_{0};
    bool prepared_{false};
};

namespace {

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
                threads_.emplace_back([self, i] {
                    self->run(i);
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
        std::vector<std::thread> joining_threads;
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
        std::vector<std::thread> retiring_threads;
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

    std::size_t thread_count_;
    std::size_t queue_capacity_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    // Queue operations move only pointers under mutex_. A claimed node stays
    // exclusive while user callable moves/destruction run outside the lock.
    local_ring_queue<node_type*> queue_;
    std::vector<node_type> nodes_;
    std::vector<std::thread> threads_;
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

post_outcome<> blocking_pool::post(move_only_function<void()> task_value) {
    if (!task_value) {
        throw std::invalid_argument("blocking pool requires a callable task");
    }
    const auto owner_ptr = impl_;
    auto& owner = *owner_ptr;
    impl_type::node_type* node = nullptr;
    std::optional<post_status> refused;
    {
        std::lock_guard lock(owner.mutex_);
        if (owner.stopping_) {
            ++owner.discarded_;
            refused = post_status::stopping;
        } else if (owner.queued_ + owner.reserved_ >= owner.queue_capacity_) {
            ++owner.rejected_;
            refused = post_status::queue_full;
        } else {
            node = owner.free_head_;
            owner.free_head_ = node->next_;
            ++owner.reserved_;
        }
    }
    // The refused task moves back to the caller outside the lock: its move
    // constructor may reenter the pool.
    if (refused) {
        return detail::delivery_access::reject<post_outcome<>>(*refused, std::move(task_value));
    }
    {
        const impl_type::reserved_post_scope reservation(owner);
        node->task_ = std::move(task_value);
    }
    bool stopped = false;
    {
        std::lock_guard lock(owner.mutex_);
        stopped = owner.stopping_;
        if (stopped) {
            ++owner.discarded_;
        } else {
            --owner.reserved_;
            if (!owner.queue_.try_push(node)) {
                std::terminate();
            }
            ++owner.queued_;
        }
    }
    if (stopped) {
        // Stopping never recycles nodes, so the reserved node stays exclusive
        // until its callable has been handed back.
        auto rejected = [&] {
            const impl_type::reserved_post_scope reservation(owner);
            return detail::delivery_access::reject<post_outcome<>>(post_status::stopping, std::move(node->task_));
        }();
        bool notify = false;
        {
            std::lock_guard lock(owner.mutex_);
            notify = (--owner.reserved_ == 0);
        }
        if (notify) {
            owner.condition_.notify_all();
        }
        return rejected;
    }
    owner.condition_.notify_one();
    return detail::delivery_access::accept<post_outcome<>>();
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
