#include "ruvia/core/blocking_pool.h"

#include <algorithm>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/core/failure_report.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/core/worker_wait.h"

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
    explicit local_ring_queue(std::size_t capacity)
        : storage_(allocate_storage(capacity)) {}

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

}  // namespace ruvia

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

thread_local const void* g_current_blocking_pool = nullptr;
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
        case blocking_status::unavailable:
            return "worker has no blocking pool";
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
          queue_(queue_capacity_),
          nodes_(),
          threads_() {
        // Reserved/queued submissions occupy at most queue_capacity_ nodes;
        // each thread may also hold one node while moving its callable out.
        nodes_.resize(queue_capacity_ + thread_count_);
        for (auto& node : nodes_) {
            node.next_ = free_head_;
            free_head_ = &node;
        }
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
        struct current_pool_guard final {
            const void* previous_;
            explicit current_pool_guard(const impl_type* pool) noexcept
                : previous_(std::exchange(g_current_blocking_pool, pool)) {}
            ~current_pool_guard() {
                g_current_blocking_pool = previous_;
            }
        } current_guard(this);
        std::unique_lock lock(mutex_);
        for (;;) {
            condition_.wait(lock, [this] { return stopping_ || queued_ != 0; });
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
            lock.unlock();

            move_only_function<void()> task = std::move(node->task_);
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

            lock.lock();
            node->next_ = free_head_;
            free_head_ = node;
            --running_;
            ++completed_;
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
        // stop(), or submit() safely without recursion.
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
        stop();
        std::vector<std::thread> joining_threads;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] { return !joining_; });
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
                return (stop_drain_thread_ == std::thread::id{} ||
                           stop_drain_thread_ == std::this_thread::get_id()) &&
                       reserved_ == 0;
            });
        }
    }

    void detach_threads() noexcept {
        std::vector<std::thread> detached_threads;
        {
            std::lock_guard lock(mutex_);
            detached_threads.swap(threads_);
        }
        for (auto& thread : detached_threads) {
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
    std::vector<node_type> nodes_;
    std::vector<std::thread> threads_;
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

blocking_pool::blocking_pool(blocking_pool_options options)
    : impl_(std::make_shared<impl_type>(options)) {
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

blocking_submit_result blocking_pool::submit(move_only_function<void()> task_value) {
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
            return detail::worker_delivery_access::reject<blocking_submit_result>(
                blocking_submit_status::pool_stopped, std::move(task_value));
        }
        if (owner.queued_ + owner.reserved_ >= owner.queue_capacity_) {
            ++owner.rejected_;
            return detail::worker_delivery_access::reject<blocking_submit_result>(
                blocking_submit_status::queue_full, std::move(task_value));
        }
        node = owner.free_head_;
        owner.free_head_ = node->next_;
        ++owner.reserved_;
    }
    node->task_ = std::move(task_value);
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
        auto rejected = detail::worker_delivery_access::reject<blocking_submit_result>(
            blocking_submit_status::pool_stopped, std::move(node->task_));
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
    return detail::worker_delivery_access::accept<blocking_submit_result>();
}

void blocking_pool::stop() noexcept {
    const auto owner = impl_;
    owner->stop();
}

void blocking_pool::join() {
    const auto owner = impl_;
    owner->join();
}

}  // namespace ruvia

namespace ruvia {

class pool_lease_scheduler::impl_type final {
public:
    using acquire_result_type = worker_wait_result<std::size_t>;

    static constexpr std::size_t no_free_slot = static_cast<std::size_t>(-1);
    static constexpr std::size_t slot_leased = static_cast<std::size_t>(-2);

    // Intrusive FIFO of one pool_lease_scheduler. The scheduler is only ever
    // touched from its worker thread, so the queue needs no synchronization.
    class waiter_queue_type final {
    public:
        void enqueue(pool_lease_acquire& waiter) noexcept {
            waiter.previous_ = tail_;
            waiter.next_ = nullptr;
            waiter.queued_ = true;
            if (tail_ != nullptr) {
                tail_->next_ = std::addressof(waiter);
            } else {
                head_ = std::addressof(waiter);
            }
            tail_ = std::addressof(waiter);
        }

        void remove(pool_lease_acquire& waiter) noexcept {
            if (!waiter.queued_) {
                return;
            }
            if (waiter.previous_ != nullptr) {
                waiter.previous_->next_ = waiter.next_;
            } else {
                head_ = waiter.next_;
            }
            if (waiter.next_ != nullptr) {
                waiter.next_->previous_ = waiter.previous_;
            } else {
                tail_ = waiter.previous_;
            }
            waiter.previous_ = nullptr;
            waiter.next_ = nullptr;
            waiter.queued_ = false;
        }

        // Commit the freed slot to the next waiter before deferring its continuation.
        // Cancellation and closure cannot replace this result after the handoff.
        [[nodiscard]] bool commit_handoff(
            std::size_t index, std::coroutine_handle<>& continuation) noexcept {
            continuation = {};
            if (head_ == nullptr) {
                return false;
            }
            continuation = head_->resolve(worker_wait_result<std::size_t>::with_value(index));
            return true;
        }

        [[nodiscard]] bool cancel(std::uint64_t id) noexcept {
            auto* waiter = find(id);
            std::coroutine_handle<> continuation;
            if (waiter == nullptr || !commit_cancellation(*waiter, continuation)) {
                return false;
            }
            if (continuation) {
                continuation.resume();
            }
            return true;
        }

        // Commit cancellation while still inside a stop callback, but let the
        // caller resume the coroutine only after that callback has returned.
        [[nodiscard]] bool commit_cancellation(
            pool_lease_acquire& waiter, std::coroutine_handle<>& continuation) noexcept {
            continuation = {};
            if (!waiter.queued_) {
                return false;
            }
            continuation = waiter.resolve(worker_wait_status::cancelled);
            return true;
        }

        [[nodiscard]] bool expire(pool_lease_acquire& waiter, worker_wait_status status) noexcept {
            if (!waiter.queued_) {
                return false;
            }
            if (auto continuation = waiter.resolve(status)) {
                continuation.resume();
            }
            return true;
        }

        // Commit closure or shutdown for the entire current queue and resume
        // every waiter in a later worker turn, as release() does: closure runs
        // inside the owner's own teardown, which must finish before any waiter
        // observes it.
        void complete_all(const worker_ref& worker, worker_wait_status status) noexcept {
            while (head_ != nullptr) {
                if (auto continuation = head_->resolve(status)) {
                    worker.defer_resume(continuation);
                }
            }
        }

    private:
        [[nodiscard]] pool_lease_acquire* find(std::uint64_t id) const noexcept {
            auto* waiter = head_;
            while (waiter != nullptr) {
                if (waiter->waiter_id_ == id) {
                    return waiter;
                }
                waiter = waiter->next_;
            }
            return nullptr;
        }

        pool_lease_acquire* head_{nullptr};
        pool_lease_acquire* tail_{nullptr};
    };

    impl_type(std::size_t pool_size, const worker_ref& worker_value)
        : worker_(worker_value),
          next_free_(pool_size, no_free_slot),
          waiter_state_(std::make_shared<waiter_state_type>(worker_value)) {
        for (std::size_t i = 0; i < pool_size; ++i) {
            next_free_[i] = free_head_;
            free_head_ = i;
        }
        if (worker_.register_shutdown(waiter_state_, &waiter_state_type::shutdown_registration_) !=
            worker_registration_status::registered) {
            waiter_state_->stopping_ = true;
        }
    }

    ~impl_type() {
        if (reserved_acquires_ != 0) {
            std::terminate();
        }
    }

    struct waiter_state_type final : std::enable_shared_from_this<waiter_state_type> {
        explicit waiter_state_type(worker_ref worker_value) noexcept
            : worker_(std::move(worker_value)) {}

        // First member: unlinks from the worker registry after every other member is gone.
        worker_shutdown_registration shutdown_registration_;
        worker_ref worker_;
        waiter_queue_type queue_;
        // Worker-affine: set once the shutdown notification reaches the worker.
        bool stopping_{false};

        void stop_waiters() noexcept {
            stopping_ = true;
            queue_.complete_all(worker_, worker_wait_status::worker_stopping);
        }

        // The notification may arrive on the stopping thread; the queue is
        // touched only on the worker.
        void worker_stopping() noexcept {
            if (worker_.is_current()) {
                stop_waiters();
                return;
            }
            try {
                // A detached worker has already retired every suspended acquire.
                (void)worker_.defer_completion([retained = shared_from_this()] { retained->stop_waiters(); });
            } catch (...) {
                std::terminate();
            }
        }

        void request_cancellation(pool_lease_acquire& waiter, std::uint64_t waiter_id) {
            if (worker_.is_current()) {
                std::coroutine_handle<> continuation;
                if (!queue_.commit_cancellation(waiter, continuation) || !continuation) {
                    return;
                }
                worker_.defer_resume(continuation);
                return;
            }
            auto retained = shared_from_this();
            // A detached worker has already retired the owners of every
            // suspended acquire, so there is nothing left to cancel.
            (void)worker_.defer_completion([retained = std::move(retained), waiter_id] {
                (void)retained->queue_.cancel(waiter_id);
            });
        }
    };

    [[nodiscard]] std::optional<acquire_result_type> try_acquire(
        const std::optional<std::chrono::steady_clock::time_point>& deadline, const stop_token& stop_token_value) {
        if (!worker_.is_current()) {
            throw std::logic_error("pool lease acquire must run on its bound worker");
        }
        if (closing_) {
            return worker_wait_result<std::size_t>::with_status(worker_wait_status::closed);
        }
        if (stop_token_value.stop_requested()) {
            return worker_wait_result<std::size_t>::with_status(worker_wait_status::cancelled);
        }
        if (free_head_ == no_free_slot) {
            if (waiter_state_->stopping_ || !worker_.accepting() ||
                detail::worker_ref_access::timers_stopping(worker_)) {
                return worker_wait_result<std::size_t>::with_status(worker_wait_status::worker_stopping);
            }
            if (deadline.has_value() && *deadline <= std::chrono::steady_clock::now()) {
                return worker_wait_result<std::size_t>::with_status(worker_wait_status::timed_out);
            }
            return std::nullopt;
        }
        const auto slot = free_head_;
        free_head_ = std::exchange(next_free_[slot], slot_leased);
        return worker_wait_result<std::size_t>::with_value(slot);
    }

    [[nodiscard]] std::uint64_t next_waiter_id() noexcept {
        auto waiter_id = ++next_waiter_id_;
        if (waiter_id == 0) {
            waiter_id = ++next_waiter_id_;
        }
        return waiter_id;
    }

    void release(std::size_t slot) noexcept {
        if (slot >= next_free_.size() || next_free_[slot] != slot_leased) {
            std::terminate();
        }
        std::coroutine_handle<> continuation;
        if (!closing_ && waiter_state_->queue_.commit_handoff(slot, continuation)) {
            if (continuation) {
                worker_.defer_resume(continuation);
            }
            return;
        }
        next_free_[slot] = free_head_;
        free_head_ = slot;
    }

    [[nodiscard]] bool close() noexcept {
        if (closing_) {
            return false;
        }
        closing_ = true;
        waiter_state_->queue_.complete_all(worker_, worker_wait_status::closed);
        return true;
    }

    worker_ref worker_;
    std::vector<std::size_t> next_free_;
    std::size_t free_head_{no_free_slot};
    std::shared_ptr<waiter_state_type> waiter_state_;
    std::size_t reserved_acquires_{0};
    std::uint64_t next_waiter_id_{0};
    bool closing_{false};
};

pool_lease_scheduler::pool_lease_scheduler(std::size_t pool_size, const worker_ref& worker_value) {
    if (!worker_value.valid()) {
        throw std::invalid_argument("pool lease scheduler requires a valid worker");
    }
    impl_ = std::make_unique<impl_type>(pool_size, worker_value);
}

pool_lease_scheduler::~pool_lease_scheduler() = default;

pool_lease_acquire pool_lease_scheduler::acquire(operation_options options) & {
    return pool_lease_acquire(*impl_, options.timeout_, std::move(options.stop_token_));
}

pool_lease_acquire::pool_lease_acquire(pool_lease_scheduler::impl_type& owner_value,
    std::optional<operation_timeout> timeout, stop_token stop_token_value) noexcept
    : owner_(&owner_value),
      timeout_(timeout),
      stop_token_(std::move(stop_token_value)) {
    ++owner_->reserved_acquires_;
}

pool_lease_acquire::pool_lease_acquire(pool_lease_acquire&& other) noexcept
    : owner_(other.awaited_ ? nullptr : std::exchange(other.owner_, nullptr)),
      timeout_(other.timeout_),
      deadline_(other.deadline_),
      stop_token_(std::move(other.stop_token_)) {
    if (other.awaited_) {
        std::terminate();
    }
}

pool_lease_acquire::~pool_lease_acquire() {
    if (owner_ != nullptr) {
        if (queued_) {
            cancel_registrations();
        }
        --owner_->reserved_acquires_;
    }
}

void pool_lease_acquire::cancel_registrations() noexcept {
    if (queued_ && owner_ != nullptr) {
        owner_->waiter_state_->queue_.remove(*this);
    }
    deadline_timer_.cancel();
    stop_registration_.reset();
}

void pool_lease_acquire::complete(worker_wait_result<std::size_t> result_value) noexcept {
    if (result_.has_value()) {
        std::terminate();
    }
    result_.emplace(result_value);
}

std::coroutine_handle<> pool_lease_acquire::resolve(worker_wait_result<std::size_t> result_value) noexcept {
    cancel_registrations();
    complete(result_value);
    return std::exchange(continuation_, {});
}

std::coroutine_handle<> pool_lease_acquire::resolve(worker_wait_status status) noexcept {
    return resolve(worker_wait_result<std::size_t>::with_status(status));
}

bool pool_lease_acquire::await_ready() {
    if (owner_ == nullptr || awaited_) {
        throw std::logic_error("pool lease acquire cannot be awaited");
    }
    awaited_ = true;
    if (timeout_.has_value()) {
        deadline_ = timeout_->deadline_from(std::chrono::steady_clock::now());
    }
    if (auto acquired = owner_->try_acquire(deadline_, stop_token_)) {
        result_.emplace(std::move(*acquired));
    }
    return result_.has_value();
}

bool pool_lease_acquire::await_suspend(std::coroutine_handle<> continuation) {
    // Nothing ran between await_ready and here, so the pool is still saturated.
    waiter_id_ = owner_->next_waiter_id();
    const auto waiter_id = waiter_id_;
    auto* const waiter_state = owner_->waiter_state_.get();
    waiter_state->queue_.enqueue(*this);

    // A deadline arms a worker-local timer owned by this acquire and cancelled
    // before it can be destroyed. Its expiry can unlink the waiter directly;
    // cross-thread stop notifications still use waiter IDs. A timer queue that
    // stops, before or after arming, ends the wait as worker_stopping.
    if (deadline_.has_value()) {
        auto status = worker_timer_schedule_status::worker_stopping;
        try {
            status = owner_->worker_.schedule_timer(deadline_timer_, *deadline_,
                [waiter_state, waiter = this](worker_timer_outcome outcome) noexcept {
                    (void)waiter_state->queue_.expire(*waiter, outcome == worker_timer_outcome::expired
                                                                   ? worker_wait_status::timed_out
                                                                   : worker_wait_status::worker_stopping);
                });
        } catch (...) {
            cancel_registrations();
            throw;
        }
        if (status == worker_timer_schedule_status::worker_stopping) {
            (void)resolve(worker_wait_status::worker_stopping);
            return false;
        }
    }

    if (stop_token_.stoppable()) {
        stop_token_.register_callback(stop_registration_,
            [waiter_state, waiter = this, waiter_id] { waiter_state->request_cancellation(*waiter, waiter_id); });
    }
    if (result_.has_value()) {
        return false;
    }
    continuation_ = continuation;
    return true;
}

worker_wait_result<std::size_t> pool_lease_acquire::await_resume() {
    if (!result_.has_value()) {
        std::terminate();
    }
    return *result_;
}

void pool_lease_scheduler::release(std::size_t slot) noexcept {
    impl_->release(slot);
}

bool pool_lease_scheduler::close() noexcept {
    return impl_->close();
}

bool pool_lease_scheduler::closing() const noexcept {
    return impl_->closing_;
}

}  // namespace ruvia
