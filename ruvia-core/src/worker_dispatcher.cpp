#include <algorithm>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/failure_report.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/core/worker_timer.h"

#include "event_loop_state.h"

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

worker_dispatcher::worker_dispatcher(asio::io_context& io_context, std::size_t capacity, ruvia::blocking_pool* pool)
    : io_context_(io_context),
      timer_(std::make_unique<asio::steady_timer>(io_context)),
      capacity_(checked_queue_capacity(capacity)),
      blocking_pool_(pool),
      nodes_(),
      worker_id_(next_worker_dispatcher_id()),
      timers_(),
      timer_slots_() {
    // At most `capacity_` nodes are pending (reserved or ready) and at most
    // `capacity_` belong to the batch drain() is running, since a batch is
    // the ready list it spliced. Twice the capacity therefore keeps the
    // free list non-empty whenever admission has room.
    nodes_.resize(capacity_ * 2);
    for (std::size_t index = 0; index + 1 < nodes_.size(); ++index) {
        nodes_[index].next_ = index + 1;
    }
    free_head_ = 0;
    timers_.reserve(capacity);
    // Timer slots are address-stable (deque); they grow on demand.
}

worker_dispatcher::~worker_dispatcher() {
    notify_waiters(idle_waiters_.take_all(), false);
    notify_waiters(shutdown_notification_waiters_.take_all(), false);
}

}  // namespace ruvia::detail

namespace ruvia {

worker_shutdown_registration::~worker_shutdown_registration() {
    if (registry_) {
        registry_->unlink_shutdown(*this);
    }
}

}  // namespace ruvia

namespace ruvia::detail {

void worker_dispatcher::schedule_drain_locked() {
    if (!drain_scheduled_) {
        asio::post(io_context_, [self = shared_from_this()] { self->drain(); });
        drain_scheduled_ = true;
    }
}

void worker_dispatcher::push_ready_locked(std::size_t index) noexcept {
    nodes_[index].next_ = no_node;
    if (ready_tail_ == no_node) {
        ready_head_ = index;
    } else {
        nodes_[ready_tail_].next_ = index;
    }
    ready_tail_ = index;
    ++ready_count_;
}

post_status worker_dispatcher::reserve(std::size_t& index) noexcept {
    std::lock_guard lock(mutex_);
    if (!context_attached_ || !accepting_) {
        return post_status::worker_stopping;
    }
    if (pending_count_ == capacity_) {
        return post_status::queue_full;
    }
    index = free_head_;
    free_head_ = nodes_[index].next_;
    ++pending_count_;
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
    std::size_t index = no_node;
    if (const auto status = reserve(index); status != post_status::accepted) {
        return detail::worker_delivery_access::reject<post_result_type>(status, std::move(task_value));
    }
    nodes_[index].task_ = std::move(task_value);
    publish(index);
    return detail::worker_delivery_access::accept<post_result_type>();
}

post_status worker_dispatcher::post_factory(move_only_function<move_only_function<void()>()> factory) {
    if (!factory) {
        throw std::invalid_argument("worker post factory requires a callable");
    }
    std::size_t index = no_node;
    if (const auto status = reserve(index); status != post_status::accepted) {
        return status;
    }
    try {
        auto task_value = factory();
        if (!task_value) {
            throw std::invalid_argument("worker post factory produced an empty task");
        }
        nodes_[index].task_ = std::move(task_value);
    } catch (...) {
        release_abandoned_node(index);
        throw;
    }
    publish(index);
    return post_status::accepted;
}

void worker_dispatcher::publish(std::size_t index) {
    std::unique_lock lock(mutex_);
    if (!abandon_drain_ && context_attached_) {
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
    auto abandoned = std::move(nodes_[index].task_);
    waiter_node_type* callbacks = nullptr;
    bool all_pending_released = false;
    {
        std::lock_guard lock(mutex_);
        nodes_[index].next_ = free_head_;
        free_head_ = index;
        all_pending_released = (--pending_count_ == 0);
        callbacks = take_idle_callbacks_locked();
    }
    abandoned = nullptr;
    notify_waiters(callbacks);
    if (all_pending_released) {
        pending_changed_.notify_all();
    }
}

void worker_dispatcher::release_abandoned_chain(std::size_t index) noexcept {
    // The spliced chain is exclusively owned here; each node stays pending
    // until release_abandoned_node returns it.
    while (index != no_node) {
        const auto next = nodes_[index].next_;
        release_abandoned_node(index);
        index = next;
    }
}

bool worker_dispatcher::defer_if_attached(move_only_function<void()> task_value) {
    if (!task_value) {
        throw std::invalid_argument("worker defer requires a callable task");
    }
    // Neither path keeps the dispatcher alive for the handler: the handler
    // only runs the task, and the io_context it is queued on outlives it.
    if (running_on_worker()) {
        asio::post(io_context_, std::move(task_value));
        return true;
    }
    // Off the worker, the mutex orders this post before detach_context(),
    // after which the context may be destroyed.
    std::lock_guard lock(mutex_);
    if (!context_attached_) {
        return false;
    }
    asio::post(io_context_, std::move(task_value));
    return true;
}

worker_registration_status worker_dispatcher::register_shutdown(worker_shutdown_registration& hook,
    std::weak_ptr<void> owner, const worker_shutdown_operations_type& operations) {
    // begin_stopping() clears accepting under the admission mutex before
    // notify_stopping() first takes this mutex on the same thread, so a
    // registration that still observes accepting here is linked before the
    // stopping batch walks the list.
    std::lock_guard lock(hooks_mutex_);
    if (hook.registry_) {
        throw std::logic_error("worker shutdown registration is already linked");
    }
    if (!accepting_) {
        return worker_registration_status::worker_stopping;
    }
    hook.operations_ = &operations;
    hook.registry_ = shared_from_this();
    hook.owner_ = std::move(owner);
    hook.previous_ = hooks_tail_;
    hook.next_ = nullptr;
    hook.linked_ = true;
    if (hooks_tail_ != nullptr) {
        hooks_tail_->next_ = &hook;
    } else {
        hooks_head_ = &hook;
    }
    hooks_tail_ = &hook;
    return worker_registration_status::registered;
}

void worker_dispatcher::unlink_shutdown(worker_shutdown_registration& hook) noexcept {
    std::lock_guard lock(hooks_mutex_);
    if (!hook.linked_) {
        return;
    }
    if (hook.previous_ != nullptr) {
        hook.previous_->next_ = hook.next_;
    } else {
        hooks_head_ = hook.next_;
    }
    if (hook.next_ != nullptr) {
        hook.next_->previous_ = hook.previous_;
    } else {
        hooks_tail_ = hook.previous_;
    }
    hook.previous_ = nullptr;
    hook.next_ = nullptr;
    hook.linked_ = false;
}

void worker_dispatcher::when_shutdown_notifications_complete(move_only_function<void()> callback_value) {
    if (!callback_value) {
        throw std::invalid_argument("shutdown notification callback requires a callable");
    }
    auto node = std::make_unique<waiter_node_type>(std::move(callback_value));
    {
        std::lock_guard lock(mutex_);
        if (shutdown_notification_active_) {
            shutdown_notification_waiters_.push_back(node.release());
            return;
        }
    }
    callback_value = std::move(node->callback_);
    node.reset();
    callback_value();
}

void worker_dispatcher::when_idle(move_only_function<void()> callback_value) {
    if (!callback_value) {
        throw std::invalid_argument("worker idle callback requires a callable");
    }
    auto node = std::make_unique<waiter_node_type>(std::move(callback_value));
    bool detached = false;
    {
        std::lock_guard lock(mutex_);
        if (!context_attached_) {
            detached = true;
        } else if (pending_count_ != 0 || active_count_ != 0) {
            idle_waiters_.push_back(node.release());
            return;
        }
    }
    callback_value = std::move(node->callback_);
    node.reset();
    if (detached) {
        throw std::runtime_error("worker execution context is detached");
    }
    callback_value();
}

worker_dispatcher::waiter_node_type* worker_dispatcher::take_idle_callbacks_locked() noexcept {
    if (pending_count_ == 0 && active_count_ == 0) {
        return idle_waiters_.take_all();
    }
    return nullptr;
}

void worker_dispatcher::notify_waiters(waiter_node_type* head, bool invoke) noexcept {
    while (head != nullptr) {
        std::unique_ptr<waiter_node_type> current(head);
        head = current->next_;
        if (!invoke) {
            continue;
        }
        try {
            current->callback_();
        } catch (...) {
            std::terminate();
        }
    }
}

void worker_dispatcher::run_context(move_only_function<void()> startup_handler,
    move_only_function<void(std::exception_ptr)> failure_handler,
    move_only_function<void()> shutdown_handler) {
    current_worker_guard current(*this);
    bool failure_delivered = false;
    std::exception_ptr deferred_failure;
    std::exception_ptr delivered_failure;
    const auto deliver_failure = [this, &failure_delivered, &deferred_failure, &delivered_failure, &failure_handler](
                                     std::exception_ptr failure) {
        notify_stopping(begin_stopping(true));
        abandon_queued();
        if (!failure_delivered) {
            failure_delivered = true;
            delivered_failure = failure;
            try {
                if (failure_handler) {
                    failure_handler(failure);
                } else {
                    deferred_failure = std::move(failure);
                }
            } catch (...) {
                deferred_failure = std::current_exception();
            }
        } else if (!equivalent_failures(failure, delivered_failure)) {
            // Only the first failure is delivered; later ones would vanish.
            report_unhandled_failure("additional worker failure", failure);
        }
        stop_timers();
    };

    try {
        if (startup_handler) {
            startup_handler();
        }
    } catch (...) {
        deliver_failure(std::current_exception());
    }
    for (;;) {
        try {
            io_context_.run();
            break;
        } catch (...) {
            deliver_failure(std::current_exception());
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
    std::vector<timer_entry> abandoned_timers;
    std::unique_ptr<asio::steady_timer> detached_timer;
    {
        std::lock_guard lock(mutex_);
        if (!context_attached_) {
            return;
        }
        accepting_ = false;
        context_attached_ = false;
        abandon_drain_ = true;
        detached_timer = std::move(timer_);
    }
    {
        // Detached hooks are never notified; their destructors find them
        // unlinked.
        std::lock_guard lock(hooks_mutex_);
        auto* hook = std::exchange(hooks_head_, nullptr);
        hooks_tail_ = nullptr;
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
    timers_stopping_.store(true, std::memory_order_release);
    abandoned_timers.swap(timers_);
    stale_timer_count_ = 0;
    timer_armed_ = false;
    for (std::size_t index = 0; index < timer_slots_.size(); ++index) {
        if (timer_slots_[index].occupied_) {
            static_cast<void>(release_timer_slot(index));
        }
    }
    detached_timer.reset();
    abandon_queued();
}

void worker_dispatcher::wait_for_reservations() noexcept {
    std::unique_lock lock(mutex_);
    pending_changed_.wait(lock, [this] { return pending_count_ == 0; });
}

bool worker_dispatcher::attached() const noexcept {
    return context_attached_.load(std::memory_order_acquire);
}

asio::io_context::executor_type worker_dispatcher::executor() const {
    const std::lock_guard lock(mutex_);
    if (!context_attached_) {
        throw std::logic_error("worker execution context is detached");
    }
    return io_context_.get_executor();
}

bool worker_dispatcher::begin_stopping(bool abandon_drain) noexcept {
    std::lock_guard lock(mutex_);
    return begin_stopping_locked(abandon_drain);
}

bool worker_dispatcher::begin_stopping_locked(bool abandon_drain) noexcept {
    if (abandon_drain) {
        drain_scheduled_ = false;
        abandon_drain_ = true;
    }
    if (!accepting_) {
        return false;
    }
    accepting_ = false;
    shutdown_notification_active_ = true;
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
        worker_shutdown_registration* hook_{nullptr};
        std::shared_ptr<void> owner_;
    };
    const auto first_live = [](worker_shutdown_registration* hook) noexcept {
        pinned_hook pinned;
        while (hook != nullptr && !(pinned.owner_ = hook->owner_.lock())) {
            hook = hook->next_;
        }
        pinned.hook_ = hook;
        return pinned;
    };
    using operations_type = worker_shutdown_operations_type;
    const auto notify_each = [this, &first_live](operations_type::phase_type operations_type::* phase) noexcept {
        pinned_hook current;
        {
            std::lock_guard lock(hooks_mutex_);
            current = first_live(hooks_head_);
        }
        while (current.owner_) {
            if (const auto callback = current.hook_->operations_->*phase) {
                callback(current.owner_.get());
            }
            pinned_hook next;
            {
                std::lock_guard lock(hooks_mutex_);
                next = first_live(current.hook_->next_);
            }
            current = std::move(next);
        }
    };
    notify_each(&operations_type::stopping_);
    notify_each(&operations_type::stopping_complete_);

    waiter_node_type* waiters = nullptr;
    {
        std::lock_guard lock(mutex_);
        if (!shutdown_notification_active_) {
            std::terminate();
        }
        shutdown_notification_active_ = false;
        waiters = shutdown_notification_waiters_.take_all();
    }
    notify_waiters(waiters);
}

void worker_dispatcher::abandon_queued() noexcept {
    std::size_t index = no_node;
    {
        std::lock_guard lock(mutex_);
        index = std::exchange(ready_head_, no_node);
        ready_tail_ = no_node;
        ready_count_ = 0;
    }
    release_abandoned_chain(index);
}

bool worker_dispatcher::is_current() const noexcept {
    if (g_current_worker != nullptr || !external_runner_.load(std::memory_order_acquire)) {
        return g_current_worker == this;
    }
    std::lock_guard lock(mutex_);
    return context_attached_ && io_context_.get_executor().running_in_this_thread();
}

bool worker_dispatcher::accepting() const noexcept {
    return accepting_.load(std::memory_order_acquire);
}

worker_id_type worker_dispatcher::id() const noexcept {
    return context_attached_.load(std::memory_order_acquire) ? worker_id_ : 0;
}

void worker_dispatcher::drain() {
    // Each handler runs one spliced batch and then returns to the io_context.
    // Work published meanwhile is left to a freshly posted handler, so I/O
    // completions, timers, and deferred continuations interleave with a busy
    // queue instead of starving behind it.
    std::size_t batch_head = no_node;
    std::size_t batch_count = 0;
    bool abandon = false;
    waiter_node_type* callbacks = nullptr;
    {
        std::lock_guard lock(mutex_);
        if (abandon_drain_) {
            drain_scheduled_ = false;
            abandon = true;
        } else if (ready_count_ == 0) {
            drain_scheduled_ = false;
        } else {
            batch_head = std::exchange(ready_head_, no_node);
            batch_count = std::exchange(ready_count_, 0);
            ready_tail_ = no_node;
            pending_count_ -= batch_count;
            active_count_ += batch_count;
        }
        if (abandon || batch_count == 0) {
            callbacks = take_idle_callbacks_locked();
        }
    }
    if (abandon) {
        notify_waiters(callbacks);
        abandon_queued();
        return;
    }
    if (batch_count == 0) {
        notify_waiters(callbacks);
        return;
    }

    std::size_t index = batch_head;
    std::size_t batch_tail = no_node;
    std::size_t completed = 0;
    while (index != no_node) {
        auto& node = nodes_[index];
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
                std::lock_guard lock(mutex_);
                node.next_ = free_head_;
                free_head_ = batch_head;
                active_count_ -= batch_count;
                pending_count_ += batch_count - completed - 1;
                batch_active = begin_stopping_locked(true);
            }
            notify_stopping(batch_active);
            release_abandoned_chain(next);
            abandon_queued();
            waiter_node_type* failure_callbacks = nullptr;
            {
                std::lock_guard lock(mutex_);
                failure_callbacks = take_idle_callbacks_locked();
            }
            notify_waiters(failure_callbacks);
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
        std::lock_guard lock(mutex_);
        nodes_[batch_tail].next_ = free_head_;
        free_head_ = batch_head;
        active_count_ -= batch_count;
        drain_scheduled_ = false;
        abandon = abandon_drain_;
        if (!abandon && ready_count_ != 0) {
            schedule_drain_locked();
        }
        callbacks = take_idle_callbacks_locked();
    }
    notify_waiters(callbacks);
    if (abandon) {
        abandon_queued();
    }
}

}  // namespace ruvia::detail

namespace ruvia {

worker_ref::worker_ref(std::shared_ptr<detail::worker_dispatcher> dispatcher) noexcept
    : dispatcher_(std::move(dispatcher)) {}

bool worker_ref::valid() const noexcept {
    return dispatcher_ && dispatcher_->attached();
}

bool worker_ref::accepting() const noexcept {
    return dispatcher_ && dispatcher_->accepting();
}

bool worker_ref::is_current() const noexcept {
    return dispatcher_ && dispatcher_->is_current();
}

worker_id_type worker_ref::id() const noexcept {
    return dispatcher_ ? dispatcher_->id() : 0;
}

blocking_pool* worker_ref::blocking_pool() const noexcept {
    return dispatcher_ ? dispatcher_->blocking_pool() : nullptr;
}

asio::io_context::executor_type worker_ref::executor() const {
    if (!dispatcher_) {
        throw std::logic_error("worker has no execution context");
    }
    return dispatcher_->executor();
}

worker_ref detail::worker_ref_access::current() noexcept {
    const auto* dispatcher = detail::current_worker_dispatcher();
    if (dispatcher == nullptr) {
        return {};
    }
    return worker_ref(std::const_pointer_cast<detail::worker_dispatcher>(dispatcher->shared_from_this()));
}

post_result_type worker_ref::post_task(move_only_function<void()> task_value) const {
    if (!task_value) {
        throw std::invalid_argument("worker post requires a callable task");
    }
    return dispatcher_ ? dispatcher_->post(std::move(task_value))
                       : detail::worker_delivery_access::reject<post_result_type>(post_status::worker_stopping, std::move(task_value));
}

void worker_ref::defer_resume(std::coroutine_handle<> continuation) const noexcept {
    if (!dispatcher_) {
        std::terminate();
    }
    dispatcher_->defer_or_terminate([continuation] { continuation.resume(); });
}

worker_ref detail::worker_ref_access::make(
    const std::shared_ptr<worker_dispatcher>& dispatcher) noexcept {
    return worker_ref(dispatcher);
}

bool worker_ref::defer_completion(move_only_function<void()> completion) const {
    if (!completion) {
        throw std::invalid_argument("worker completion requires a callable task");
    }
    // The completion's move may release the last owner of this worker_ref.
    const auto dispatcher = dispatcher_;
    return dispatcher && dispatcher->defer_if_attached(std::move(completion));
}

void detail::worker_ref_access::defer_or_terminate(
    const worker_ref& worker_value, move_only_function<void()> task_value) noexcept {
    if (!worker_value.dispatcher_) {
        std::terminate();
    }
    worker_value.dispatcher_->defer_or_terminate(std::move(task_value));
}

worker_registration_status worker_ref::link_shutdown(worker_shutdown_registration& registration,
    std::weak_ptr<void> owner, const detail::worker_shutdown_operations_type& operations) const {
    if (!dispatcher_) {
        return worker_registration_status::worker_stopping;
    }
    return dispatcher_->register_shutdown(registration, std::move(owner), operations);
}

void detail::worker_ref_access::when_shutdown_notifications_complete(
    const worker_ref& worker_value, move_only_function<void()> callback_value) {
    const auto& dispatcher = worker_value.dispatcher_;
    if (!dispatcher) {
        throw std::runtime_error("cannot observe shutdown notifications on a stopped worker");
    }
    dispatcher->when_shutdown_notifications_complete(std::move(callback_value));
}

void detail::worker_ref_access::when_idle(
    const worker_ref& worker_value, move_only_function<void()> callback_value) {
    const auto& dispatcher = worker_value.dispatcher_;
    if (!dispatcher) {
        throw std::runtime_error("cannot observe idleness on a stopped worker");
    }
    dispatcher->when_idle(std::move(callback_value));
}

void detail::worker_ref_access::wait_for_reservations(const worker_ref& worker_value) noexcept {
    if (worker_value.dispatcher_) {
        worker_value.dispatcher_->wait_for_reservations();
    }
}

bool detail::worker_ref_access::timers_stopping(const worker_ref& worker_value) noexcept {
    return !worker_value.dispatcher_ || worker_value.dispatcher_->timers_stopping();
}

void detail::worker_ref_access::enable_external_runner(const worker_ref& worker_value) noexcept {
    if (worker_value.dispatcher_) {
        worker_value.dispatcher_->enable_external_runner();
    }
}

worker_timer_schedule_status worker_ref::schedule_timer(worker_timer_registration& registration,
    std::chrono::steady_clock::time_point deadline_value,
    move_only_function<void(worker_timer_outcome)> completion) const& {
    if (!dispatcher_) {
        throw std::runtime_error("cannot schedule a timer on a stopped worker");
    }
    return dispatcher_->schedule_timer(registration, deadline_value, std::move(completion));
}

post_status worker_ref::post_factory(move_only_function<move_only_function<void()>()> factory) const {
    const auto dispatcher = dispatcher_;
    return dispatcher ? dispatcher->post_factory(std::move(factory)) : post_status::worker_stopping;
}

}  // namespace ruvia

// The dispatcher's timer heap: registering a deadline, cancelling one from any
// thread, re-arming the single asio timer, and firing everything due on the
// worker.

namespace ruvia {

void worker_timer_cancellation::cancel() const noexcept {
    if (dispatcher_ != nullptr && generation_ != 0) {
        dispatcher_->request_timer_cancellation(slot_, generation_, true);
    }
}

worker_timer_registration::~worker_timer_registration() {
    unregister(false);
}

void worker_timer_registration::cancel() noexcept {
    unregister(false);
}

void worker_timer_registration::cancel_and_notify() noexcept {
    unregister(true);
}

void worker_timer_registration::unregister(bool notify) noexcept {
    auto* dispatcher = std::exchange(dispatcher_, nullptr);
    const auto* slot_state = std::exchange(slot_state_, nullptr);
    const auto slot = std::exchange(slot_, 0);
    const auto generation = std::exchange(generation_, 0);
    if (dispatcher == nullptr || slot_state == nullptr ||
        slot_state->active_generation_.load(std::memory_order_acquire) != generation) {
        return;
    }
    if (!notify && detail::current_worker_dispatcher() != dispatcher) {
        auto expected = generation;
        if (!const_cast<detail::timer_slot*>(slot_state)->active_generation_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }
    }
    dispatcher->request_timer_cancellation(slot, generation, notify);
}

bool worker_timer_registration::registered() const noexcept {
    return dispatcher_ != nullptr;
}

worker_timer_cancellation worker_timer_registration::cancellation() const& {
    if (dispatcher_ == nullptr || generation_ == 0) {
        throw std::logic_error("worker timer registration is not active");
    }
    return worker_timer_cancellation(*dispatcher_, slot_, generation_);
}

void worker_timer_registration::bind(detail::worker_dispatcher& dispatcher, std::size_t slot,
    std::uint64_t generation, const detail::timer_slot& slot_state) noexcept {
    dispatcher_ = &dispatcher;
    slot_state_ = &slot_state;
    slot_ = slot;
    generation_ = generation;
}

void worker_timer_registration::release() noexcept {
    dispatcher_ = nullptr;
    slot_state_ = nullptr;
    slot_ = 0;
    generation_ = 0;
}

bool worker_timer_registration::pending() const noexcept {
    // Pairs with the release store that deactivates the slot on its own worker
    // before expiry or cancellation delivers a completion.
    return slot_state_ != nullptr && generation_ != 0 &&
           slot_state_->active_generation_.load(std::memory_order_acquire) == generation_;
}

// Only a cold operation may move; a started one is referenced by its timer
// callback and must stay address-stable.
sleep_operation::sleep_operation(sleep_operation&& other) noexcept
    : worker_(std::exchange(other.worker_, nullptr)),
      target_(other.target_),
      stop_token_(std::move(other.stop_token_)) {
    if (other.started_) {
        std::terminate();
    }
}

bool sleep_operation::await_ready() {
    if (worker_ == nullptr || started_) {
        throw std::logic_error("sleep operation cannot be awaited");
    }
    if (!worker_->is_current()) {
        throw std::logic_error("sleep must run on its bound worker");
    }
    started_ = true;
    if (stop_token_.stop_requested()) {
        return true;
    }
    if (const auto* duration = std::get_if<timeout_duration>(&target_)) {
        return duration->value() <= std::chrono::milliseconds::zero();
    }
    return std::get<std::chrono::steady_clock::time_point>(target_) <= std::chrono::steady_clock::now();
}

// A stopped timer queue resolves immediately with the outcome stop_timers()
// would have delivered to a pending sleep.
bool sleep_operation::await_suspend(std::coroutine_handle<> continuation) {
    continuation_ = continuation;
    const auto deadline = [&] {
        if (const auto* duration = std::get_if<timeout_duration>(&target_)) {
            return deadline_after(duration->value());
        }
        return std::get<std::chrono::steady_clock::time_point>(target_);
    }();
    const auto status = worker_->schedule_timer(registration_, deadline,
        [this](worker_timer_outcome value) {
            outcome_ = value;
            continuation_.resume();
        });
    if (status == worker_timer_schedule_status::worker_stopping) {
        outcome_ = worker_timer_outcome::cancelled;
        return false;
    }
    if (stop_token_.stoppable()) {
        stop_token_.register_callback(stop_registration_,
            [cancellation = registration_.cancellation()] { cancellation.cancel(); });
    }
    return true;
}

// A zero/negative duration never suspends and keeps the default expired
// outcome, so it reports elapsed unless stop was already requested.
timer_sleep_result sleep_operation::await_resume() const noexcept {
    return outcome_ == worker_timer_outcome::expired && !stop_token_.stop_requested()
               ? timer_sleep_result::elapsed
               : timer_sleep_result::stop_requested;
}

sleep_operation sleep_for(const worker_ref& worker_value, timeout_duration duration, stop_token stop_token_value) {
    return sleep_operation(worker_value, duration, std::move(stop_token_value));
}

sleep_operation sleep_until(const worker_ref& worker_value,
    std::chrono::steady_clock::time_point deadline, stop_token stop_token_value) {
    return sleep_operation(worker_value, deadline, std::move(stop_token_value));
}
}  // namespace ruvia

namespace ruvia::detail {

worker_timer_schedule_status worker_dispatcher::schedule_timer(worker_timer_registration& registration,
    std::chrono::steady_clock::time_point deadline_value,
    move_only_function<void(worker_timer_outcome)> completion) {
    if (!is_current()) {
        throw std::logic_error("worker timers must be scheduled on their worker");
    }
    if (registration.scheduling_) {
        throw std::logic_error("worker timer registration is already active");
    }
    registration.scheduling_ = true;
    struct scheduling_guard final {
        bool& scheduling_;
        ~scheduling_guard() {
            scheduling_ = false;
        }
    } guard{registration.scheduling_};
    if (registration.dispatcher_ != nullptr) {
        // The previous binding may belong to another worker; its slot state is
        // the only part of that worker read here.
        if (registration.pending()) {
            throw std::logic_error("worker timer registration is already active");
        }
        registration.release();
    }
    // Detachment also stops the timer queue, so one check covers both.
    if (timers_stopping_.load(std::memory_order_acquire)) {
        return worker_timer_schedule_status::worker_stopping;
    }
    // New due entries belong to the next dispatch round. Place them after all
    // entries due at this round's snapshot, even if their requested deadline
    // is earlier, so the sequence gate cannot hide other pending expirations.
    if (dispatching_timers_) {
        deadline_value = std::max(deadline_value, timer_round_deadline_);
    }

    std::size_t slot_index = free_timer_slot_;
    if (slot_index == no_timer_slot) {
        slot_index = timer_slots_.size();
        timer_slots_.emplace_back();
    } else {
        free_timer_slot_ = timer_slots_[slot_index].next_free_;
    }
    auto& slot = timer_slots_[slot_index];
    if (++slot.generation_ == 0) {
        ++slot.generation_;
    }
    slot.next_free_ = no_timer_slot;
    slot.cancel_requested_ = false;
    // The target's move may stop timers reentrantly. Keep this reserved slot
    // unpublished until the move completes so stop_timers() cannot retire it
    // while its callback is still being constructed.
    slot.completion_ = std::move(completion);
    if (timers_stopping_.load(std::memory_order_acquire)) {
        static_cast<void>(release_timer_slot(slot_index));
        return worker_timer_schedule_status::worker_stopping;
    }
    slot.occupied_ = true;
    slot.active_generation_.store(slot.generation_, std::memory_order_release);
    try {
        timers_.push_back(timer_entry{
            .deadline_ = deadline_value,
            .sequence_ = next_timer_sequence_++,
            .slot_ = slot_index,
            .generation_ = slot.generation_,
        });
    } catch (...) {
        // Roll the slot back; the moved-in completion is destroyed uninvoked.
        static_cast<void>(release_timer_slot(slot_index));
        throw;
    }
    std::ranges::push_heap(timers_, timer_entry_later{});
    const auto generation = slot.generation_;
    registration.bind(*this, slot_index, generation, slot);
    // The armed wait already covers every deadline at or after it.
    if (!dispatching_timers_ && (!timer_armed_ || deadline_value < armed_deadline_)) {
        try {
            arm_timer();
        } catch (...) {
            // Arming cancelled the previous wait before failing. Withdraw the
            // new entry so the caller sees a clean failure, then re-arm for the
            // entries that were pending before this call.
            registration.release();
            static_cast<void>(take_timer(slot_index, generation));
            try {
                arm_timer();
            } catch (...) {
                fail_timers("worker timer arm", std::current_exception());
            }
            throw;
        }
    }
    return worker_timer_schedule_status::scheduled;
}

void worker_dispatcher::request_timer_cancellation(
    std::size_t slot, std::uint64_t generation, bool notify) noexcept {
    if (current_worker_dispatcher() != this) {
        std::lock_guard lock(mutex_);
        if (timers_stopping_.load(std::memory_order_acquire) || !context_attached_) {
            return;
        }
        if (!io_context_.get_executor().running_in_this_thread()) {
            try {
                // A stopped io_context can be restarted; queue the cancellation
                // so a destroyed registration cannot leave a live slot that
                // expires after that restart. The handler already runs fresh on
                // the worker, exactly like fire_timers(), so it delivers the
                // completion inline instead of posting a second hop.
                asio::post(io_context_, [self = shared_from_this(), slot, generation, notify] {
                    auto completion = self->take_timer(slot, generation);
                    if (notify && completion) {
                        completion(worker_timer_outcome::cancelled);
                    }
                });
            } catch (...) {
                std::terminate();
            }
            return;
        }
    }
    if (!notify) {
        static_cast<void>(take_timer(slot, generation));
        return;
    }
    if (!has_occupied_timer(slot, generation)) {
        return;
    }
    auto& slot_state = timer_slots_[slot];
    if (slot_state.cancel_requested_) {
        return;
    }
    slot_state.cancel_requested_ = true;
    note_stale_timer();
    // The caller is running synchronously on the worker; delivering its own
    // completion here would re-enter it, so the completion is posted. Deferring
    // slot extraction lets a subsequent registration.cancel() or destruction on
    // this worker withdraw the callback before the posted turn runs.
    defer_or_terminate([self = shared_from_this(), slot, generation] {
        auto completion = self->take_timer(slot, generation);
        if (completion) {
            completion(worker_timer_outcome::cancelled);
        }
    });
}

void worker_dispatcher::pop_stale_timer_top() noexcept {
    while (!timers_.empty() &&
           !has_live_timer_entry(timers_.front().slot_, timers_.front().generation_)) {
        std::ranges::pop_heap(timers_, timer_entry_later{});
        timers_.pop_back();
        if (stale_timer_count_ != 0) {
            --stale_timer_count_;
        }
    }
}

void worker_dispatcher::note_stale_timer() noexcept {
    ++stale_timer_count_;
    if (stale_timer_count_ == timers_.size()) {
        // Nothing live remains: drop the heap and the armed wait so an idle
        // worker holds no timer work.
        timers_.clear();
        release_drained_timer_heap();
        stale_timer_count_ = 0;
        if (timer_armed_ && timer_) {
            ++timer_generation_;
            timer_armed_ = false;
            timer_->cancel();
        }
    } else {
        pop_stale_timer_top();
        if (stale_timer_count_ >= 64 && stale_timer_count_ * 2 >= timers_.size()) {
            std::erase_if(timers_, [this](const timer_entry& entry_value) {
                return !has_live_timer_entry(entry_value.slot_, entry_value.generation_);
            });
            std::ranges::make_heap(timers_, timer_entry_later{});
            stale_timer_count_ = 0;
        }
    }
}

void worker_dispatcher::release_drained_timer_heap() noexcept {
    // Slots stay: registrations keep their addresses and (slot, generation)
    // pairs across threads, so only the heap, which nothing references, shrinks.
    // The heap regrows geometrically on the next burst.
    if (timers_.empty() && timers_.capacity() > capacity_) {
        std::vector<timer_entry>().swap(timers_);
    }
}

move_only_function<void(worker_timer_outcome)> worker_dispatcher::take_timer(
    std::size_t slot_index, std::uint64_t generation) noexcept {
    if (!has_occupied_timer(slot_index, generation)) {
        return {};
    }
    const bool active = has_timer(slot_index, generation);
    const bool already_stale = timer_slots_[slot_index].cancel_requested_;
    auto completion = release_timer_slot(slot_index);
    if (!already_stale) {
        note_stale_timer();
    }
    return active ? std::move(completion) : move_only_function<void(worker_timer_outcome)>{};
}

void worker_dispatcher::stop_timers() noexcept {
    if (timers_stopping_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    ++timer_generation_;
    timer_armed_ = false;
    if (timer_) {
        timer_->cancel();
        // The timer object is bound to the worker io_context service.  A
        // worker_ref may keep the dispatcher endpoint alive after shutdown,
        // so stopping timers must release the Asio object while the context is
        // still known to be alive rather than leaving that work to the
        // dispatcher's eventual destructor.
        timer_.reset();
    }

    timers_.clear();
    release_drained_timer_heap();
    stale_timer_count_ = 0;
    for (std::size_t index = 0; index < timer_slots_.size(); ++index) {
        if (!timer_slots_[index].occupied_) {
            continue;
        }
        const bool active =
            timer_slots_[index].active_generation_.load(std::memory_order_acquire) != 0;
        auto completion = release_timer_slot(index);
        try {
            if (active && completion) {
                completion(worker_timer_outcome::cancelled);
            }
        } catch (...) {
            std::terminate();
        }
    }
}

void worker_dispatcher::arm_timer() {
    ++timer_generation_;
    const auto generation = timer_generation_;
    if (!timer_) {
        return;
    }
    if (timer_armed_) {
        timer_->cancel();
        timer_armed_ = false;
    }
    pop_stale_timer_top();
    if (timers_stopping_.load(std::memory_order_acquire) || timers_.empty()) {
        release_drained_timer_heap();
        return;
    }
    armed_deadline_ = timers_.front().deadline_;
    timer_->expires_at(armed_deadline_);
    timer_->async_wait([self = shared_from_this(), generation](const std::error_code& error) {
        if (error || generation != self->timer_generation_) {
            return;
        }
        self->timer_armed_ = false;
        self->fire_timers();
    });
    // Initiation may allocate and throw; only a successful wait is armed.
    timer_armed_ = true;
}

void worker_dispatcher::fire_timers() {
    std::exception_ptr failure;
    dispatching_timers_ = true;
    // One pass fires only what was due when it began. A completion that
    // schedules a deadline already in the past (a periodic timer whose work
    // outlasts its interval) waits for the re-armed wait, so I/O, deferred work,
    // and stop controls run between rounds instead of starving behind them.
    const auto now = std::chrono::steady_clock::now();
    timer_round_deadline_ = now;
    const auto sequence_limit = next_timer_sequence_;
    try {
        for (;;) {
            pop_stale_timer_top();
            if (timers_.empty() || timers_.front().deadline_ > now ||
                timers_.front().sequence_ >= sequence_limit) {
                break;
            }
            std::ranges::pop_heap(timers_, timer_entry_later{});
            auto entry_value = std::move(timers_.back());
            timers_.pop_back();
            const bool active = has_timer(entry_value.slot_, entry_value.generation_);
            auto completion = release_timer_slot(entry_value.slot_);
            if (active && completion) {
                completion(worker_timer_outcome::expired);
            }
        }
    } catch (...) {
        failure = std::current_exception();
    }
    // Restore the queue before propagating a user callback failure. An external
    // io_context owner may catch it and continue running the remaining timers.
    dispatching_timers_ = false;
    try {
        arm_timer();
    } catch (...) {
        fail_timers("worker timer re-arm", std::current_exception());
        if (!failure) {
            throw;
        }
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

// Without an armed wait nothing pending could ever fire. Stopping the queue
// delivers every pending completion as cancelled instead of stalling silently.
void worker_dispatcher::fail_timers(std::string_view context_value, std::exception_ptr failure) noexcept {
    report_unhandled_failure(context_value, std::move(failure));
    stop_timers();
}

bool worker_dispatcher::has_timer(std::size_t slot, std::uint64_t generation) const noexcept {
    return generation != 0 && slot < timer_slots_.size() &&
           timer_slots_[slot].active_generation_.load(std::memory_order_acquire) == generation;
}

bool worker_dispatcher::has_occupied_timer(std::size_t slot, std::uint64_t generation) const noexcept {
    return generation != 0 && slot < timer_slots_.size() && timer_slots_[slot].occupied_ &&
           timer_slots_[slot].generation_ == generation;
}

bool worker_dispatcher::has_live_timer_entry(std::size_t slot, std::uint64_t generation) const noexcept {
    return has_occupied_timer(slot, generation) && !timer_slots_[slot].cancel_requested_;
}

move_only_function<void(worker_timer_outcome)> worker_dispatcher::release_timer_slot(
    std::size_t slot_index) noexcept {
    auto& slot = timer_slots_[slot_index];
    slot.occupied_ = false;
    slot.cancel_requested_ = false;
    auto completion = std::move(slot.completion_);
    slot.next_free_ = free_timer_slot_;
    free_timer_slot_ = slot_index;
    slot.active_generation_.store(0, std::memory_order_release);
    return completion;
}

}  // namespace ruvia::detail
