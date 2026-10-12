#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

#include "worker_dispatcher_impl.h"
namespace ruvia::detail {
namespace {

std::atomic<worker_id_type> g_next_worker_id{1};
thread_local const worker_dispatcher* g_current_worker = nullptr;

class current_worker_guard final {
public:
    explicit current_worker_guard(const worker_dispatcher& worker_value)
        : previous_(std::exchange(g_current_worker, &worker_value)) {
        if (previous_ != nullptr && previous_ != &worker_value) {
            g_current_worker = previous_;
            throw std::logic_error("one thread cannot run multiple Ruvia workers concurrently");
        }
    }

    ~current_worker_guard() {
        g_current_worker = previous_;
    }

    current_worker_guard(const current_worker_guard&) = delete;
    current_worker_guard& operator=(const current_worker_guard&) = delete;

private:
    const worker_dispatcher* previous_;
};

}  // namespace

const worker_dispatcher* current_worker_dispatcher() noexcept {
    return g_current_worker;
}

worker_id_type next_worker_dispatcher_id() noexcept {
    return g_next_worker_id.fetch_add(1, std::memory_order_relaxed);
}

worker_dispatcher::worker_dispatcher(asio::io_context& io_context, std::size_t capacity)
    : io_context_(io_context),
      impl_(std::make_unique<impl_type>(io_context, capacity)) {}

worker_dispatcher::~worker_dispatcher() = default;

worker_shutdown_hook::~worker_shutdown_hook() {
    if (registry_) {
        registry_->unlink_shutdown_hook(*this);
    }
}

void worker_dispatcher::schedule_drain_locked() {
    if (!impl_->drain_scheduled_) {
        asio::post(io_context_, [self = shared_from_this()] { self->drain(); });
        impl_->drain_scheduled_ = true;
    }
}

void worker_dispatcher::push_ready_locked(std::size_t index) noexcept {
    impl_->nodes_[index].next_ = impl_type::no_node;
    if (impl_->ready_tail_ == impl_type::no_node) {
        impl_->ready_head_ = index;
    } else {
        impl_->nodes_[impl_->ready_tail_].next_ = index;
    }
    impl_->ready_tail_ = index;
    ++impl_->ready_count_;
}

post_status worker_dispatcher::reserve(std::size_t& index) noexcept {
    std::lock_guard lock(impl_->mutex_);
    if (!impl_->context_attached_ || !impl_->accepting_) {
        return post_status::worker_stopping;
    }
    if (impl_->pending_count_ == impl_->capacity_) {
        return post_status::queue_full;
    }
    index = impl_->free_head_;
    impl_->free_head_ = impl_->nodes_[index].next_;
    ++impl_->pending_count_;
    return post_status::accepted;
}

// Moving a task into its node runs the callable's own move constructor, which
// may reenter this worker (post, close, detach). Admission and publication are
// therefore two critical sections with the move between them, never under the
// mutex; the reserved node counts against capacity throughout.
post_result_type worker_dispatcher::post(move_only_function<void()> task_value) {
    if (!task_value) {
        throw std::invalid_argument("worker post requires a callable task");
    }
    std::size_t index = impl_type::no_node;
    if (const auto status = reserve(index); status != post_status::accepted) {
        return post_result_type::reject(status, std::move(task_value));
    }
    impl_->nodes_[index].task_ = std::move(task_value);
    publish(index);
    return post_result_type::accept();
}

post_status worker_dispatcher::post_factory(move_only_function<move_only_function<void()>()> factory) {
    if (!factory) {
        throw std::invalid_argument("worker post factory requires a callable");
    }
    std::size_t index = impl_type::no_node;
    if (const auto status = reserve(index); status != post_status::accepted) {
        return status;
    }
    try {
        auto task_value = factory();
        if (!task_value) {
            throw std::invalid_argument("worker post factory produced an empty task");
        }
        impl_->nodes_[index].task_ = std::move(task_value);
    } catch (...) {
        release_abandoned_node(index);
        throw;
    }
    publish(index);
    return post_status::accepted;
}

void worker_dispatcher::publish(std::size_t index) {
    std::unique_lock lock(impl_->mutex_);
    if (!impl_->abandon_drain_ && impl_->context_attached_) {
        try {
            schedule_drain_locked();
        } catch (...) {
            lock.unlock();
            release_abandoned_node(index);
            throw;
        }
        push_ready_locked(index);
        return;
    }
    lock.unlock();
    release_abandoned_node(index);
}

void worker_dispatcher::release_abandoned_node(std::size_t index) noexcept {
    // The caller exclusively claimed this pending node. Move its payload out
    // before returning the slot; user destruction can reenter admission or
    // reconcile higher-level work, and must run unlocked before idle callbacks.
    auto abandoned = std::move(impl_->nodes_[index].task_);
    idle_callbacks_type callbacks{process_resource()};
    {
        std::lock_guard lock(impl_->mutex_);
        impl_->nodes_[index].next_ = impl_->free_head_;
        impl_->free_head_ = index;
        --impl_->pending_count_;
        callbacks = take_idle_callbacks_locked();
    }
    abandoned = nullptr;
    notify_idle(std::move(callbacks));
    impl_->pending_changed_.notify_all();
}

void worker_dispatcher::defer(move_only_function<void()> task_value) {
    if (!task_value) {
        throw std::invalid_argument("worker defer requires a callable task");
    }
    // Neither path keeps the dispatcher alive for the handler: the handler
    // only runs the task, and the io_context it is queued on outlives it.
    if (running_on_worker()) {
        asio::post(io_context_, std::move(task_value));
        return;
    }
    // Off the worker, the mutex orders this post before detach_context(),
    // after which the context may be destroyed.
    std::lock_guard lock(impl_->mutex_);
    if (!impl_->context_attached_) {
        throw std::runtime_error("worker execution context is detached");
    }
    asio::post(io_context_, std::move(task_value));
}

bool worker_dispatcher::defer_if_attached(move_only_function<void()> task_value) {
    if (!task_value) {
        throw std::invalid_argument("worker defer requires a callable task");
    }
    if (running_on_worker()) {
        asio::post(io_context_, std::move(task_value));
        return true;
    }
    std::lock_guard lock(impl_->mutex_);
    if (!impl_->context_attached_) {
        return false;
    }
    asio::post(io_context_, std::move(task_value));
    return true;
}

void worker_dispatcher::register_shutdown_hook(
    worker_shutdown_hook& hook, std::weak_ptr<void> owner, const worker_shutdown_hook::operations_type& operations) {
    // begin_stopping() clears accepting under the admission mutex before
    // notify_stopping() first takes this mutex on the same thread, so a
    // registration that still observes accepting here is linked before the
    // stopping batch walks the list.
    std::lock_guard lock(impl_->hooks_mutex_);
    if (!impl_->accepting_) {
        throw std::runtime_error("cannot register state on a stopping worker");
    }
    if (hook.registry_) {
        throw std::logic_error("worker shutdown hook is already registered");
    }
    hook.operations_ = &operations;
    hook.registry_ = shared_from_this();
    hook.owner_ = std::move(owner);
    hook.previous_ = impl_->hooks_tail_;
    hook.next_ = nullptr;
    hook.linked_ = true;
    if (impl_->hooks_tail_ != nullptr) {
        impl_->hooks_tail_->next_ = &hook;
    } else {
        impl_->hooks_head_ = &hook;
    }
    impl_->hooks_tail_ = &hook;
}

void worker_dispatcher::unlink_shutdown_hook(worker_shutdown_hook& hook) noexcept {
    std::lock_guard lock(impl_->hooks_mutex_);
    if (!hook.linked_) {
        return;
    }
    if (hook.previous_ != nullptr) {
        hook.previous_->next_ = hook.next_;
    } else {
        impl_->hooks_head_ = hook.next_;
    }
    if (hook.next_ != nullptr) {
        hook.next_->previous_ = hook.previous_;
    } else {
        impl_->hooks_tail_ = hook.previous_;
    }
    hook.previous_ = nullptr;
    hook.next_ = nullptr;
    hook.linked_ = false;
}

void worker_dispatcher::when_shutdown_notifications_complete(move_only_function<void()> callback_value) {
    if (!callback_value) {
        throw std::invalid_argument("shutdown notification callback requires a callable");
    }
    bool ready = false;
    {
        std::lock_guard lock(impl_->mutex_);
        if (impl_->shutdown_notification_active_) {
            impl_->shutdown_notification_waiters_.push_back(std::move(callback_value));
        } else {
            ready = true;
        }
    }
    if (ready) {
        callback_value();
    }
}

void worker_dispatcher::when_idle(move_only_function<void()> callback_value) {
    if (!callback_value) {
        throw std::invalid_argument("worker idle callback requires a callable");
    }
    bool ready = false;
    {
        std::lock_guard lock(impl_->mutex_);
        if (!impl_->context_attached_) {
            throw std::runtime_error("worker execution context is detached");
        }
        ready = impl_->pending_count_ == 0 && impl_->active_count_ == 0;
        if (!ready) {
            impl_->idle_waiters_.push_back(std::move(callback_value));
        }
    }
    if (ready) {
        callback_value();
    }
}

worker_dispatcher::idle_callbacks_type worker_dispatcher::take_idle_callbacks_locked() {
    idle_callbacks_type callbacks{process_resource()};
    if (impl_->pending_count_ == 0 && impl_->active_count_ == 0) {
        callbacks.swap(impl_->idle_waiters_);
    }
    return callbacks;
}

void worker_dispatcher::notify_idle(idle_callbacks_type callbacks) noexcept {
    for (auto& callback : callbacks) {
        try {
            callback();
        } catch (...) {
            std::terminate();
        }
    }
}

void worker_dispatcher::run_context() {
    std::exception_ptr failure;
    run_context({}, [&failure](std::exception_ptr value) noexcept { failure = std::move(value); }, {});
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

void worker_dispatcher::run_context(move_only_function<void()> startup_handler,
    move_only_function<void(std::exception_ptr)> failure_handler,
    move_only_function<void()> shutdown_handler) {
    current_worker_guard current(*this);
    bool failure_delivered = false;
    std::exception_ptr deferred_failure;
    const auto handle_failure = [this, &failure_delivered, &deferred_failure, &failure_handler](
                                    std::exception_ptr failure) {
        notify_stopping(begin_stopping(true));
        abandon_queued();
        if (!failure_delivered) {
            failure_delivered = true;
            try {
                if (failure_handler) {
                    failure_handler(failure);
                } else {
                    deferred_failure = std::move(failure);
                }
            } catch (...) {
                deferred_failure = std::current_exception();
            }
        }
        stop_timers();
    };

    try {
        if (startup_handler) {
            startup_handler();
        }
    } catch (...) {
        handle_failure(std::current_exception());
    }
    for (;;) {
        try {
            io_context_.run();
            break;
        } catch (...) {
            handle_failure(std::current_exception());
        }
    }
    if (shutdown_handler) {
        try {
            shutdown_handler();
        } catch (...) {
            std::terminate();
        }
    }
    if (deferred_failure != nullptr) {
        std::rethrow_exception(deferred_failure);
    }
}

void worker_dispatcher::close() noexcept {
    notify_stopping(begin_stopping(false));
}

void worker_dispatcher::detach_context() noexcept {
    std::pmr::vector<timer_entry> abandoned_timers(impl_->timers_.get_allocator());
    std::unique_ptr<asio::steady_timer> detached_timer;
    {
        std::lock_guard lock(impl_->mutex_);
        if (!impl_->context_attached_) {
            return;
        }
        impl_->accepting_ = false;
        impl_->context_attached_ = false;
        impl_->abandon_drain_ = true;
        detached_timer = std::move(impl_->timer_);
    }
    {
        // Detached hooks are never notified; their destructors find them
        // unlinked.
        std::lock_guard lock(impl_->hooks_mutex_);
        auto* hook = std::exchange(impl_->hooks_head_, nullptr);
        impl_->hooks_tail_ = nullptr;
        while (hook != nullptr) {
            auto* next = hook->next_;
            hook->previous_ = nullptr;
            hook->next_ = nullptr;
            hook->linked_ = false;
            hook = next;
        }
    }

    // The caller guarantees no worker-thread timer activity runs concurrently
    // with detach_context: event_loop_pool joins its threads first, an attached
    // loop invokes this from its terminal context handler, and the external
    // context service invokes it while the context is shutting down. The timer
    // heap is therefore exclusively owned here. All user-owned closures are
    // destroyed outside the mutex so a destructor that releases another worker
    // primitive cannot deadlock. Slots stay in place: bound registrations may
    // still read their activity until the dispatcher itself is destroyed.
    impl_->timers_stopping_.store(true, std::memory_order_release);
    abandoned_timers.swap(impl_->timers_);
    impl_->stale_timer_count_ = 0;
    impl_->timer_armed_ = false;
    for (std::size_t index = 0; index < impl_->timer_slots_.size(); ++index) {
        if (impl_->timer_slots_[index].active_generation_.load(std::memory_order_relaxed) != 0) {
            static_cast<void>(release_timer_slot(index));
        }
    }
    detached_timer.reset();
    abandon_queued();
}

void worker_dispatcher::wait_for_reservations() noexcept {
    std::unique_lock lock(impl_->mutex_);
    impl_->pending_changed_.wait(lock, [this] { return impl_->pending_count_ == 0; });
}

bool worker_dispatcher::attached() const noexcept {
    return impl_->context_attached_.load(std::memory_order_acquire);
}

bool worker_dispatcher::begin_stopping(bool abandon_drain) noexcept {
    std::lock_guard lock(impl_->mutex_);
    return begin_stopping_locked(abandon_drain);
}

bool worker_dispatcher::begin_stopping_locked(bool abandon_drain) noexcept {
    if (abandon_drain) {
        impl_->drain_scheduled_ = false;
        impl_->abandon_drain_ = true;
    }
    if (!impl_->accepting_) {
        return false;
    }
    impl_->accepting_ = false;
    impl_->shutdown_notification_active_ = true;
    return true;
}

void worker_dispatcher::notify_stopping(bool batch_active) noexcept {
    if (!batch_active) {
        return;
    }
    // No hook can join once accepting is false, so the batch is the registry
    // itself. Each step pins the owner of the hook it notifies, which keeps
    // that node linked while the mutex is released for the callback; an owner
    // already being destroyed is skipped. References are dropped outside the
    // mutex because the last one runs the hook's unlink.
    struct pinned_hook final {
        worker_shutdown_hook* hook_{nullptr};
        std::shared_ptr<void> owner_;
    };
    const auto first_live = [](worker_shutdown_hook* hook) noexcept {
        pinned_hook pinned;
        while (hook != nullptr && !(pinned.owner_ = hook->owner_.lock())) {
            hook = hook->next_;
        }
        pinned.hook_ = hook;
        return pinned;
    };
    using operations_type = worker_shutdown_hook::operations_type;
    const auto notify_each = [this, &first_live](operations_type::phase_type operations_type::* phase) noexcept {
        pinned_hook current;
        {
            std::lock_guard lock(impl_->hooks_mutex_);
            current = first_live(impl_->hooks_head_);
        }
        while (current.owner_) {
            if (const auto callback = current.hook_->operations_->*phase) {
                callback(current.owner_.get());
            }
            pinned_hook next;
            {
                std::lock_guard lock(impl_->hooks_mutex_);
                next = first_live(current.hook_->next_);
            }
            current = std::move(next);
        }
    };
    notify_each(&operations_type::stopping_);
    notify_each(&operations_type::stopping_complete_);

    idle_callbacks_type waiters{process_resource()};
    {
        std::lock_guard lock(impl_->mutex_);
        if (!impl_->shutdown_notification_active_) {
            std::terminate();
        }
        impl_->shutdown_notification_active_ = false;
        waiters.swap(impl_->shutdown_notification_waiters_);
    }
    for (auto& waiter : waiters) {
        try {
            waiter();
        } catch (...) {
            std::terminate();
        }
    }
}

void worker_dispatcher::abandon_queued() noexcept {
    std::size_t index = impl_type::no_node;
    {
        std::lock_guard lock(impl_->mutex_);
        index = std::exchange(impl_->ready_head_, impl_type::no_node);
        impl_->ready_tail_ = impl_type::no_node;
        impl_->ready_count_ = 0;
    }
    // The spliced chain is exclusively owned here; each node stays pending
    // until release_abandoned_node returns it.
    while (index != impl_type::no_node) {
        const auto next = impl_->nodes_[index].next_;
        release_abandoned_node(index);
        index = next;
    }
}

bool worker_dispatcher::is_current() const noexcept {
    if (g_current_worker == this) {
        return true;
    }
    std::lock_guard lock(impl_->mutex_);
    return impl_->context_attached_ && io_context_.get_executor().running_in_this_thread();
}

bool worker_dispatcher::accepting() const noexcept {
    return impl_->accepting_.load(std::memory_order_acquire);
}

worker_id_type worker_dispatcher::id() const noexcept {
    return impl_->context_attached_.load(std::memory_order_acquire) ? impl_->worker_id_ : 0;
}

void worker_dispatcher::drain() {
    // Each handler runs one spliced batch and then returns to the io_context.
    // Work published meanwhile is left to a freshly posted handler, so I/O
    // completions, timers, and deferred continuations interleave with a busy
    // queue instead of starving behind it.
    std::size_t batch_head = impl_type::no_node;
    std::size_t batch_count = 0;
    bool abandon = false;
    idle_callbacks_type callbacks{process_resource()};
    {
        std::lock_guard lock(impl_->mutex_);
        if (impl_->abandon_drain_) {
            impl_->drain_scheduled_ = false;
            abandon = true;
        } else if (impl_->ready_count_ == 0) {
            impl_->drain_scheduled_ = false;
        } else {
            batch_head = std::exchange(impl_->ready_head_, impl_type::no_node);
            batch_count = std::exchange(impl_->ready_count_, 0);
            impl_->ready_tail_ = impl_type::no_node;
            impl_->pending_count_ -= batch_count;
            impl_->active_count_ += batch_count;
        }
        callbacks = take_idle_callbacks_locked();
    }
    notify_idle(std::move(callbacks));
    if (abandon) {
        abandon_queued();
        return;
    }
    if (batch_count == 0) {
        return;
    }

    std::size_t index = batch_head;
    std::size_t batch_tail = impl_type::no_node;
    std::size_t completed = 0;
    while (index != impl_type::no_node) {
        auto& node = impl_->nodes_[index];
        const auto next = node.next_;
        try {
            node.task_();
        } catch (...) {
            node.task_ = nullptr;
            bool batch_active = false;
            {
                // The completed prefix through the failed node returns to
                // the free list; the unstarted remainder is pending again
                // and is released below like queued work. The remainder can
                // push pending past capacity, so admission closes in this
                // same critical section before any reservation observes it.
                std::lock_guard lock(impl_->mutex_);
                node.next_ = impl_->free_head_;
                impl_->free_head_ = batch_head;
                impl_->active_count_ -= batch_count;
                impl_->pending_count_ += batch_count - completed - 1;
                batch_active = begin_stopping_locked(true);
            }
            notify_stopping(batch_active);
            for (auto remaining = next; remaining != impl_type::no_node;) {
                const auto following = impl_->nodes_[remaining].next_;
                release_abandoned_node(remaining);
                remaining = following;
            }
            abandon_queued();
            idle_callbacks_type failure_callbacks{process_resource()};
            {
                std::lock_guard lock(impl_->mutex_);
                failure_callbacks = take_idle_callbacks_locked();
            }
            notify_idle(std::move(failure_callbacks));
            throw;
        }
        node.task_ = nullptr;
        ++completed;
        batch_tail = index;
        index = next;
    }

    {
        // The finished batch returns to the free list. A post failure leaves
        // drain unscheduled and escapes as a worker failure, whose handling
        // abandons the ready list.
        std::lock_guard lock(impl_->mutex_);
        impl_->nodes_[batch_tail].next_ = impl_->free_head_;
        impl_->free_head_ = batch_head;
        impl_->active_count_ -= batch_count;
        impl_->drain_scheduled_ = false;
        abandon = impl_->abandon_drain_;
        if (!abandon && impl_->ready_count_ != 0) {
            schedule_drain_locked();
        }
        callbacks = take_idle_callbacks_locked();
    }
    notify_idle(std::move(callbacks));
    if (abandon) {
        abandon_queued();
    }
}

}  // namespace ruvia::detail
