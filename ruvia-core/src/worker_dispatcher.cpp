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
    : impl_(std::make_unique<impl_type>(io_context, capacity)) {}

worker_dispatcher::~worker_dispatcher() = default;

post_result_type worker_dispatcher::post(move_only_function<void()> task_value) {
    if (!task_value) {
        throw std::invalid_argument("worker post requires a callable task");
    }
    std::size_t index = no_timer_slot;
    post_status rejection = post_status::accepted;
    {
        std::lock_guard lock(impl_->mutex_);
        if (!impl_->context_attached_ || !impl_->accepting_) {
            rejection = post_status::worker_stopping;
        } else if (impl_->pending_count_ == impl_->nodes_.size() - 1 ||
                   impl_->free_head_ == no_timer_slot) {
            rejection = post_status::queue_full;
        } else {
            index = impl_->free_head_;
            impl_->free_head_ = impl_->nodes_[index].next_;
            impl_->nodes_[index].state_ = impl_type::node_state_type::reserved;
            ++impl_->pending_count_;
        }
    }
    if (rejection != post_status::accepted) {
        return post_result_type::reject(rejection, std::move(task_value));
    }
    impl_->nodes_[index].task_ = std::move(task_value);
    try {
        publish(index);
    } catch (...) {
        rollback_reserved(index);
        throw;
    }
    return post_result_type::accept();
}

post_status worker_dispatcher::post_factory(move_only_function<move_only_function<void()>()> factory) {
    if (!factory) {
        throw std::invalid_argument("worker post factory requires a callable");
    }
    std::size_t index = no_timer_slot;
    {
        std::lock_guard lock(impl_->mutex_);
        if (!impl_->context_attached_ || !impl_->accepting_) {
            return post_status::worker_stopping;
        }
        if (impl_->pending_count_ == impl_->nodes_.size() - 1 ||
            impl_->free_head_ == no_timer_slot) {
            return post_status::queue_full;
        }
        index = impl_->free_head_;
        impl_->free_head_ = impl_->nodes_[index].next_;
        impl_->nodes_[index].state_ = impl_type::node_state_type::reserved;
        ++impl_->pending_count_;
    }
    try {
        auto task_value = factory();
        if (!task_value) {
            throw std::invalid_argument("worker post factory produced an empty task");
        }
        impl_->nodes_[index].task_ = std::move(task_value);
        publish(index);
    } catch (...) {
        rollback_reserved(index);
        throw;
    }
    return post_status::accepted;
}

void worker_dispatcher::publish(std::size_t index) {
    bool abandon = false;
    {
        auto queue = impl_->ready_queue_.lock();
        if (impl_->abandon_drain_ || !impl_->context_attached_) {
            impl_->nodes_[index].state_ = impl_type::node_state_type::releasing;
            abandon = true;
        } else {
            if (!impl_->drain_scheduled_) {
                asio::post(impl_->io_context_, [self = shared_from_this()] { self->drain(); });
                impl_->drain_scheduled_ = true;
            }
            impl_->nodes_[index].state_ = impl_type::node_state_type::ready;
            if (!queue.try_push(index)) {
                std::terminate();
            }
        }
    }
    if (abandon) {
        release_abandoned_node(index);
    }
}

void worker_dispatcher::rollback_reserved(std::size_t index) noexcept {
    {
        std::lock_guard lock(impl_->mutex_);
        if (impl_->nodes_[index].state_ != impl_type::node_state_type::reserved) {
            return;
        }
        impl_->nodes_[index].state_ = impl_type::node_state_type::releasing;
    }
    release_abandoned_node(index);
}

void worker_dispatcher::release_abandoned_node(std::size_t index) noexcept {
    // The caller exclusively claimed this node as releasing. Move its payload
    // out before returning the slot; user destruction can reenter admission or
    // reconcile higher-level work, and must run unlocked before idle callbacks.
    auto abandoned = std::move(impl_->nodes_[index].task_);
    idle_callbacks_type callbacks{process_resource()};
    {
        std::lock_guard lock(impl_->mutex_);
        impl_->nodes_[index].state_ = impl_type::node_state_type::free;
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
    std::lock_guard lock(impl_->mutex_);
    if (!impl_->context_attached_) {
        throw std::runtime_error("worker execution context is detached");
    }
    asio::post(impl_->io_context_,
        [self = shared_from_this(), task_value = std::move(task_value)]() mutable { task_value(); });
}

bool worker_dispatcher::defer_if_attached(move_only_function<void()> task_value) {
    if (!task_value) {
        throw std::invalid_argument("worker defer requires a callable task");
    }
    std::lock_guard lock(impl_->mutex_);
    if (!impl_->context_attached_) {
        return false;
    }
    asio::post(impl_->io_context_,
        [self = shared_from_this(), task_value = std::move(task_value)]() mutable { task_value(); });
    return true;
}

void worker_dispatcher::defer_or_terminate(move_only_function<void()> task_value) noexcept {
    try {
        defer(std::move(task_value));
    } catch (...) {
        std::terminate();
    }
}

void worker_dispatcher::register_shutdown_listener(
    const std::shared_ptr<worker_shutdown_listener>& listener_value) {
    std::lock_guard lock(impl_->mutex_);
    if (!impl_->accepting_) {
        throw std::runtime_error("cannot register state on a stopping worker");
    }
    std::erase_if(impl_->shutdown_listeners_, [](const auto& entry_value) { return entry_value.expired(); });
    impl_->shutdown_listeners_.emplace_back(listener_value);
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
    run_context([&failure](std::exception_ptr value) noexcept { failure = std::move(value); });
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

void worker_dispatcher::run_context(move_only_function<void(std::exception_ptr)> failure_handler) {
    run_context({}, std::move(failure_handler), {});
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
            impl_->io_context_.run();
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
    shutdown_listeners_type abandoned_listeners{process_resource()};
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
        abandoned_listeners.swap(impl_->shutdown_listeners_);
        detached_timer = std::move(impl_->timer_);
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

worker_dispatcher::shutdown_batch_type worker_dispatcher::begin_stopping(bool abandon_drain) noexcept {
    shutdown_batch_type batch;
    {
        std::lock_guard lock(impl_->mutex_);
        if (abandon_drain) {
            impl_->drain_scheduled_ = false;
            impl_->abandon_drain_ = true;
        }
        if (!impl_->accepting_) {
            return batch;
        }
        impl_->accepting_ = false;
        impl_->shutdown_notification_active_ = true;
        batch.listeners_.swap(impl_->shutdown_listeners_);
        batch.active_ = true;
    }
    return batch;
}

void worker_dispatcher::notify_stopping(shutdown_batch_type batch) noexcept {
    if (!batch.active_) {
        return;
    }
    for (const auto& entry : batch.listeners_) {
        if (const auto listener = entry.lock()) {
            listener->worker_stopping();
        }
    }
    for (const auto& entry : batch.listeners_) {
        if (const auto listener = entry.lock()) {
            listener->worker_stopping_complete();
        }
    }

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
    for (;;) {
        std::size_t index = no_timer_slot;
        {
            auto queue = impl_->ready_queue_.lock();
            if (!queue.try_pop(index)) {
                return;
            }
            impl_->nodes_[index].state_ = impl_type::node_state_type::releasing;
        }
        release_abandoned_node(index);
    }
}

bool worker_dispatcher::is_current() const noexcept {
    if (g_current_worker == this) {
        return true;
    }
    std::lock_guard lock(impl_->mutex_);
    return impl_->context_attached_ && impl_->io_context_.get_executor().running_in_this_thread();
}

bool worker_dispatcher::accepting() const noexcept {
    return impl_->accepting_.load(std::memory_order_acquire);
}

worker_id_type worker_dispatcher::id() const noexcept {
    return impl_->context_attached_.load(std::memory_order_acquire) ? impl_->worker_id_ : 0;
}

void worker_dispatcher::drain() {
    for (;;) {
        move_only_function<void()> task;
        std::size_t index = no_timer_slot;
        {
            auto queue = impl_->ready_queue_.lock();
            if (impl_->abandon_drain_) {
                impl_->drain_scheduled_ = false;
                break;
            }
            if (!queue.try_pop(index)) {
                impl_->drain_scheduled_ = false;
                return;
            }
            impl_->nodes_[index].state_ = impl_type::node_state_type::active;
            --impl_->pending_count_;
            ++impl_->active_count_;
        }
        task = std::move(impl_->nodes_[index].task_);
        try {
            task();
        } catch (...) {
            task = nullptr;
            {
                std::lock_guard lock(impl_->mutex_);
                impl_->nodes_[index].state_ = impl_type::node_state_type::free;
                impl_->nodes_[index].next_ = impl_->free_head_;
                impl_->free_head_ = index;
                --impl_->active_count_;
            }
            notify_stopping(begin_stopping(true));
            abandon_queued();
            idle_callbacks_type callbacks{process_resource()};
            {
                std::lock_guard lock(impl_->mutex_);
                callbacks = take_idle_callbacks_locked();
            }
            notify_idle(std::move(callbacks));
            throw;
        }
        task = nullptr;
        idle_callbacks_type callbacks{process_resource()};
        {
            std::lock_guard lock(impl_->mutex_);
            impl_->nodes_[index].state_ = impl_type::node_state_type::free;
            impl_->nodes_[index].next_ = impl_->free_head_;
            impl_->free_head_ = index;
            --impl_->active_count_;
            callbacks = take_idle_callbacks_locked();
        }
        notify_idle(std::move(callbacks));
    }
    abandon_queued();
}

}  // namespace ruvia::detail
