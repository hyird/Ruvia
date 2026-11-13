#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

#include <asio/config.hpp>

#include "ruvia/core/worker_ref.h"
#if defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm>
#include <new>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <asio/bind_executor.hpp>
#include <asio/execution_context.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/core/worker_ref.h"

#include "event_loop_state.h"

namespace ruvia {

namespace {

class runtime_lifecycle final {
public:
    [[nodiscard]] bool start() noexcept {
        auto expected = worker_runtime_state::ready;
        return state_.compare_exchange_strong(
            expected, worker_runtime_state::running, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    [[nodiscard]] bool request_stop() noexcept {
        auto observed_value = state_.load(std::memory_order_acquire);
        while (observed_value == worker_runtime_state::ready || observed_value == worker_runtime_state::running) {
            if (state_.compare_exchange_weak(observed_value, worker_runtime_state::stopping, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    void complete_stop() noexcept {
        auto expected = worker_runtime_state::stopping;
        (void)state_.compare_exchange_strong(
            expected, worker_runtime_state::stopped, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    [[nodiscard]] worker_runtime_state state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

private:
    std::atomic<worker_runtime_state> state_{worker_runtime_state::ready};
};

// The first failure of a runtime owner, kept until the owner rethrows it or
// hands it to the unhandled-failure diagnostic. Later failures lose. The owner
// guards it with its own lifecycle lock so recording and waking its waiters
// stay one critical section.
class first_failure final {
public:
    // Returns true for the winning failure. `observed` records that a consumer
    // (such as a failure hook) already owns it.
    [[nodiscard]] bool record(std::exception_ptr failure, bool observed) noexcept {
        if (failure == nullptr || first_ != nullptr) {
            return false;
        }
        first_ = std::move(failure);
        observed_ = observed;
        return true;
    }

    [[nodiscard]] const std::exception_ptr& first() const noexcept {
        return first_;
    }

    [[nodiscard]] bool failed() const noexcept {
        return first_ != nullptr;
    }

    // Returns the first failure, if any, for the caller to rethrow.
    [[nodiscard]] std::exception_ptr observe() noexcept {
        observed_ = true;
        return first_;
    }

    // Releases a first failure nobody observed; the caller must rethrow or
    // report it. A later failure may then become the next first one.
    [[nodiscard]] std::exception_ptr take_unobserved() noexcept {
        if (observed_) {
            return {};
        }
        return std::exchange(first_, nullptr);
    }

private:
    std::exception_ptr first_;
    bool observed_{false};
};
}  // namespace
class worker_runtime::impl_type final {
public:
    enum class phase_type : std::uint8_t { pending,
        reached,
        aborted };

    explicit impl_type(worker_runtime_options options)
        : io_context_(
              asio::config_from_concurrency_hint(options.io_policy_ == worker_io_policy::single_owner
                                                     ? ASIO_CONCURRENCY_HINT_UNSAFE_IO
                                                     : ASIO_CONCURRENCY_HINT_DEFAULT)),
          context_(io_context_, options.queue_capacity_, options.blocking_pool_),
          stop_(context_) {
        stop_.configure({
            .stop_admission_ = [this] { begin_stop_admission(); },
            // The hook's target may reenter finalize() while it moves or dies.
            .finalize_ =
                [this] {
                    auto hook = std::move(hooks_.finalize_);
                    if (hook) {
                        hook();
                    }
                },
        });
    }

    // Returns true for the winning failure.
    [[nodiscard]] bool record_failure(std::exception_ptr failure, bool observed) noexcept {
        {
            const std::lock_guard lock(mutex_);
            if (!failure_.record(failure, observed)) {
                return false;
            }
        }
        // Publish the cause before stopping another member wakes a group phase
        // waiter. The user's failure hook remains a separate notification.
        notify_group(std::move(failure));
        phase_changed_.notify_all();
        return true;
    }

    void bind_group(worker_group& group, worker_runtime& member) {
        const std::lock_guard lock(mutex_);
        if (group_ != nullptr || group_notifications_ != 0 || failure_.failed() ||
            lifecycle_.state() != worker_runtime_state::ready) {
            throw std::logic_error("worker group member must be cold and belong to one group");
        }
        group_ = &group;
        group_member_ = &member;
    }

    void unbind_group(worker_group& group) noexcept {
        std::unique_lock lock(mutex_);
        if (group_ != &group) {
            std::terminate();
        }
        group_ = nullptr;
        group_member_ = nullptr;
        condition_.wait(lock, [this] { return group_notifications_ == 0; });
    }

    void notify_group(std::exception_ptr failure = {}) noexcept {
        worker_group* group = nullptr;
        worker_runtime* member = nullptr;
        {
            const std::lock_guard lock(mutex_);
            group = group_;
            if (group == nullptr) {
                return;
            }
            member = group_member_;
            ++group_notifications_;
        }
        // Group stop may reenter this runtime or notify other members. Keep no
        // runtime lock across it, and retire the observation before user hooks.
        if (failure) {
            group->member_failed(*member, std::move(failure));
        } else {
            group->member_stopping();
        }
        const std::lock_guard lock(mutex_);
        --group_notifications_;
        condition_.notify_all();
    }

    bool fail(std::exception_ptr failure) noexcept {
        const bool first = record_failure(failure, static_cast<bool>(hooks_.failure_));
        if (failure != nullptr) {
            request_stop();
        }
        if (first && hooks_.failure_) {
            try {
                hooks_.failure_(std::move(failure));
            } catch (...) {
                std::terminate();
            }
        }
        return first;
    }

    [[nodiscard]] std::exception_ptr unobserved_failure() noexcept {
        const std::lock_guard lock(mutex_);
        return failure_.take_unobserved();
    }

    void configure(worker_runtime_hooks hooks) {
        {
            const std::lock_guard lock(mutex_);
            if (configured_ || lifecycle_.state() != worker_runtime_state::ready) {
                throw std::logic_error("worker runtime hooks must be configured once before launch");
            }
            configured_ = true;
        }
        // Configuration precedes publication and launch. The reservation
        // rejects reentry; callable moves run without the runtime mutex.
        hooks_ = std::move(hooks);
    }

    // Owner thread, once, from the stop chain. A runtime with a loop first
    // drains the loop's retirement gate, which then resumes here through
    // finish_loop_stop_admission().
    void begin_stop_admission() {
        std::shared_ptr<detail::event_loop_state> loop;
        {
            const std::lock_guard lock(mutex_);
            loop = loop_;
            stop_admission_delegated_ = loop != nullptr;
        }
        if (loop) {
            loop->complete_stop_admission();
            return;
        }
        run_stop_admission();
    }

    void finish_loop_stop_admission() noexcept {
        {
            const std::lock_guard lock(mutex_);
            if (!std::exchange(stop_admission_delegated_, false)) {
                return;
            }
        }
        run_stop_admission();
    }

    void run_stop_admission() noexcept {
        try {
            if (hooks_.stop_admission_) {
                hooks_.stop_admission_();
            } else {
                finalize();
            }
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] std::shared_ptr<detail::event_loop_state> loop(worker_runtime& owner) {
        {
            const std::lock_guard lock(mutex_);
            if (loop_) {
                return loop_;
            }
        }
        auto created = std::make_shared<detail::event_loop_state>(owner);
        created->failure_sink_ = [&owner](std::exception_ptr failure) { (void)owner.fail(std::move(failure)); };
        {
            const std::lock_guard lock(mutex_);
            if (loop_) {
                return loop_;
            }
            loop_ = created;
        }
        // A worker that already stops never notifies the loop; retire its
        // admission directly.
        if (context_.worker().register_shutdown(created, &detail::event_loop_state::shutdown_registration_) !=
            worker_registration_status::registered) {
            created->stop();
        }
        return created;
    }

    [[nodiscard]] bool start() {
        std::unique_lock lock(mutex_);
        if (start_called_) {
            throw std::logic_error("worker runtime can only be started once");
        }
        start_called_ = true;
        if (!lifecycle_.start()) {
            return false;
        }
        try {
            thread_ = std::thread([this] { run(true); });
        } catch (...) {
            const auto failure = std::current_exception();
            lock.unlock();
            (void)record_failure(failure, true);
            request_stop();
            std::rethrow_exception(failure);
        }
        return true;
    }

    void mark_ready() noexcept {
        {
            const std::lock_guard lock(mutex_);
            if (ready_ != phase_type::pending ||
                lifecycle_.state() != worker_runtime_state::running) {
                return;
            }
            ready_ = phase_type::reached;
        }
        phase_changed_.notify_all();
    }

    [[nodiscard]] bool wait_for(const phase_type& phase) {
        std::unique_lock lock(mutex_);
        if (lifecycle_.state() == worker_runtime_state::ready) {
            throw std::logic_error("worker runtime has not been launched");
        }
        phase_changed_.wait(lock, [&] { return phase != phase_type::pending || failure_.failed(); });
        if (const auto failure = failure_.observe()) {
            std::rethrow_exception(failure);
        }
        return phase == phase_type::reached;
    }

    void request_serve() {
        {
            const std::lock_guard lock(mutex_);
            if (ready_ == phase_type::pending) {
                throw std::logic_error("worker runtime must be ready before it serves");
            }
            if (ready_ != phase_type::reached || serve_requested_) {
                return;
            }
            serve_requested_ = true;
        }
        // A refused control means stopping already aborted the serving phase.
        (void)post_control([this] { apply_serve(); });
    }

    void apply_serve() noexcept {
        if (lifecycle_.state() != worker_runtime_state::running) {
            return;
        }
        try {
            if (hooks_.serve_) {
                hooks_.serve_();
            }
        } catch (...) {
            (void)fail(std::current_exception());
            return;
        }
        {
            const std::lock_guard lock(mutex_);
            if (serving_ != phase_type::pending ||
                lifecycle_.state() != worker_runtime_state::running) {
                return;
            }
            serving_ = phase_type::reached;
        }
        phase_changed_.notify_all();
    }

    void abort_phases() noexcept {
        {
            const std::lock_guard lock(mutex_);
            if (ready_ == phase_type::pending) {
                ready_ = phase_type::aborted;
            }
            if (serving_ == phase_type::pending) {
                serving_ = phase_type::aborted;
            }
        }
        phase_changed_.notify_all();
    }

    void request_stop() noexcept {
        bool dispatch = false;
        {
            const std::lock_guard lock(mutex_);
            dispatch = lifecycle_.request_stop();
            if (dispatch) {
                stop_dispatching_ = true;
                stop_dispatch_thread_ = std::this_thread::get_id();
            }
        }
        if (dispatch) {
            // Publish independent stop before shutdown listeners can reenter
            // group stop. Its self-stop recursion closes admission through the
            // same nonwaiting path as every other repeat caller.
            notify_group();
        }
        // Losing the lifecycle transition does not imply its winner has
        // closed admission yet: it may still be notifying a group.
        // Every caller establishes the closed gate.
        context_.close();
        if (!dispatch) {
            return;
        }
        stop_.request_stop();
        abort_phases();
#if defined(_WIN32)
        thread_join_operation* waiters;
#endif
        {
            const std::lock_guard lock(mutex_);
            stop_dispatching_ = false;
            stop_dispatch_thread_ = {};
#if defined(_WIN32)
            waiters = std::exchange(thread_waiters_, nullptr);
#endif
        }
        condition_.notify_all();
#if defined(_WIN32)
        while (waiters) {
            auto* current = waiters;
            waiters = current->next_;
            current->next_ = nullptr;
            auto caller = current->caller_;
            const auto continuation = std::exchange(current->continuation_, {});
            caller.defer_resume(continuation);
        }
#endif
    }

    [[nodiscard]] bool post_control(move_only_function<void()> control) noexcept {
        if (lifecycle_.state() != worker_runtime_state::running) {
            return false;
        }
        return stop_.post_control(std::move(control));
    }

    void finalize() noexcept {
        request_stop();
        stop_.finalize();
    }

    void run(bool launch_startup) noexcept {
        try {
            context_.run(
                [this, launch_startup] {
                    if (launch_startup && hooks_.startup_) {
                        hooks_.startup_();
                    }
                },
                [this](std::exception_ptr failure) noexcept { (void)fail(std::move(failure)); },
                [this] {
                    if (hooks_.shutdown_) {
                        hooks_.shutdown_();
                    }
                });
        } catch (...) {
            (void)record_failure(std::current_exception(), false);
        }
        // Detach before the context or domain owners can disappear. Unfinished
        // post factories own their stable endpoint, not this execution context.
        context_.detach();
        (void)lifecycle_.request_stop();
        abort_phases();
        lifecycle_.complete_stop();
    }

#if defined(_WIN32)
    std::uintptr_t prepare_thread_join() {
        if (context_.worker().is_current()) {
            throw std::logic_error("cannot join a worker runtime from its owner");
        }
        request_stop();
        const std::lock_guard lock(mutex_);
        if (joining_ || stop_dispatching_) {
            throw std::logic_error("cannot prepare a worker thread wait while joining or dispatching its stop hooks");
        }
        if (joined_) {
            return 0;
        }
        if (!thread_.joinable()) {
            thread_ = std::thread([this] { run(false); });
        }
        HANDLE duplicate{};
        if (!::DuplicateHandle(::GetCurrentProcess(), thread_.native_handle(),
                ::GetCurrentProcess(), &duplicate, SYNCHRONIZE, FALSE, 0)) {
            const auto error = ::GetLastError();
            if (!std::in_range<int>(error)) {
                throw std::runtime_error("worker thread handle duplication returned an out-of-range system error");
            }
            throw std::system_error(static_cast<int>(error), std::system_category(),
                "failed to duplicate worker thread wait handle");
        }
        return reinterpret_cast<std::uintptr_t>(duplicate);
    }
#endif

    void join() {
        if (context_.worker().is_current()) {
            throw std::logic_error("cannot join a worker runtime from its owner");
        }
        request_stop();
        std::thread joining_thread;
        bool drain_unstarted = false;
        {
            std::unique_lock lock(mutex_);
            if (stop_dispatching_ && stop_dispatch_thread_ == std::this_thread::get_id()) {
                throw std::logic_error("cannot join a worker runtime while dispatching its stop hooks");
            }
            // Wait for an external stop dispatch before establishing an owner
            // and draining its work, even when our stop lost its transition.
            condition_.wait(lock, [this] { return !joining_ && !stop_dispatching_; });
            if (joined_) {
                return;
            }
            joining_ = true;
            if (thread_.joinable()) {
                joining_thread = std::move(thread_);
            } else {
                drain_unstarted = true;
            }
        }
        if (drain_unstarted) {
            try {
                joining_thread = std::thread([this] { run(false); });
            } catch (...) {
                // Thread creation failure does not waive accepted work or
                // owner-affine cleanup. Establish owner identity synchronously.
                (void)record_failure(std::current_exception(), false);
                run(false);
            }
        }
        if (joining_thread.joinable()) {
            joining_thread.join();
        }
        {
            const std::lock_guard lock(mutex_);
            joining_ = false;
            joined_ = true;
        }
        condition_.notify_all();
    }

    asio::io_context io_context_;
    worker_runtime_context context_;
    detail::worker_stop_chain stop_;
    runtime_lifecycle lifecycle_;
    worker_runtime_hooks hooks_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::condition_variable phase_changed_;
    std::thread thread_;
    first_failure failure_;
    phase_type ready_{phase_type::pending};
    phase_type serving_{phase_type::pending};
    bool serve_requested_{};
    bool configured_{};
    bool start_called_{};
    bool stop_dispatching_{};
    std::thread::id stop_dispatch_thread_{};
#if defined(_WIN32)
    thread_join_operation* thread_waiters_{};
#endif
    bool joining_{};
    bool joined_{};
    worker_group* group_{nullptr};
    worker_runtime* group_member_{nullptr};
    std::size_t group_notifications_{0};
    std::shared_ptr<detail::event_loop_state> loop_;
    bool stop_admission_delegated_{false};
};

worker_runtime::worker_runtime(worker_runtime_options options)
    : impl_(std::make_unique<impl_type>(options)) {}

worker_runtime::~worker_runtime() {
    impl_->request_stop();
    try {
        impl_->join();
    } catch (...) {
        // An owner cannot safely detach a running thread to recover self-join.
        std::terminate();
    }
    if (const auto failure = impl_->unobserved_failure()) {
        report_unhandled_failure("worker runtime", failure);
    }
}

void worker_runtime::configure(worker_runtime_hooks hooks) {
    impl_->configure(std::move(hooks));
}

void worker_runtime::bind_group(worker_group& group) {
    impl_->bind_group(group, *this);
}

void worker_runtime::unbind_group(worker_group& group) noexcept {
    impl_->unbind_group(group);
}

bool worker_runtime::start() {
    return impl_->start();
}

void worker_runtime::mark_ready() noexcept {
    impl_->mark_ready();
}

bool worker_runtime::wait_until_ready() {
    return impl_->wait_for(impl_->ready_);
}

void worker_runtime::request_serve() {
    impl_->request_serve();
}

bool worker_runtime::wait_until_serving() {
    return impl_->wait_for(impl_->serving_);
}

void worker_runtime::request_stop() noexcept {
    impl_->request_stop();
}

bool worker_runtime::fail(std::exception_ptr failure) noexcept {
    return impl_->fail(std::move(failure));
}

bool worker_runtime::post_control(move_only_function<void()> control) noexcept {
    return impl_->post_control(std::move(control));
}

void worker_runtime::finalize() noexcept {
    impl_->finalize();
}

void worker_runtime::join() {
    impl_->join();
}
#if defined(_WIN32)
worker_runtime::thread_join_operation::thread_join_operation(worker_runtime& owner, worker_ref caller) noexcept
    : owner_(&owner),
      caller_(std::move(caller)) {}

worker_runtime::thread_join_operation::~thread_join_operation() {
    if (started_ && !resumed_) {
        std::terminate();
    }
}

bool worker_runtime::thread_join_operation::await_ready() {
    if (!caller_.is_current()) {
        throw std::logic_error("worker thread join preparation must run on its caller worker");
    }
    if (owner_->worker().is_current()) {
        throw std::logic_error("cannot join a worker runtime from its owner");
    }
    owner_->request_stop();
    const std::lock_guard lock(owner_->impl_->mutex_);
    if (owner_->impl_->joining_ || (owner_->impl_->stop_dispatching_ &&
                                       owner_->impl_->stop_dispatch_thread_ == std::this_thread::get_id())) {
        throw std::logic_error("cannot prepare a worker thread wait during join or its own stop dispatch");
    }
    started_ = true;
    return !owner_->impl_->stop_dispatching_;
}

bool worker_runtime::thread_join_operation::await_suspend(std::coroutine_handle<> continuation) {
    const std::lock_guard lock(owner_->impl_->mutex_);
    if (!owner_->impl_->stop_dispatching_) {
        return false;
    }
    continuation_ = continuation;
    next_ = owner_->impl_->thread_waiters_;
    owner_->impl_->thread_waiters_ = this;
    return true;
}

std::uintptr_t worker_runtime::thread_join_operation::await_resume() {
    resumed_ = true;
    return owner_->impl_->prepare_thread_join();
}

worker_runtime::thread_join_operation worker_runtime::prepare_thread_join(worker_ref caller) & {
    return thread_join_operation(*this, std::move(caller));
}
#endif

std::exception_ptr worker_runtime::take_failure() noexcept {
    const std::lock_guard lock(impl_->mutex_);
    return impl_->failure_.observe();
}

std::exception_ptr worker_runtime::recorded_failure() const noexcept {
    const std::lock_guard lock(impl_->mutex_);
    return impl_->failure_.first();
}

worker_runtime_state worker_runtime::state() const noexcept {
    return impl_->lifecycle_.state();
}

asio::io_context& worker_runtime::io_context() & noexcept {
    return impl_->io_context_;
}

const worker_ref& worker_runtime::worker() const& noexcept {
    return impl_->context_.worker();
}

event_loop worker_runtime::loop() & {
    return event_loop(impl_->loop(*this));
}

void worker_runtime::finish_loop_stop_admission() noexcept {
    impl_->finish_loop_stop_admission();
}

worker_runtime_context& worker_runtime::context() & noexcept {
    return impl_->context_;
}

worker_runtime_context::worker_runtime_context(
    asio::io_context& io_context, std::size_t queue_capacity, blocking_pool* pool)
    : io_context_(&io_context),
      dispatcher_(std::make_shared<detail::worker_dispatcher>(io_context, queue_capacity, pool)),
      worker_(detail::worker_ref_access::make(dispatcher_)) {}

worker_runtime_context::~worker_runtime_context() {
    detach();
}

asio::io_context& worker_runtime_context::io_context() const& noexcept {
    return *io_context_;
}

const worker_ref& worker_runtime_context::worker() const& noexcept {
    return worker_;
}

void worker_runtime_context::run() {
    dispatcher_->run_context({}, {}, {});
}

void worker_runtime_context::run(move_only_function<void()> startup_handler,
    move_only_function<void(std::exception_ptr)> failure_handler,
    move_only_function<void()> shutdown_handler) {
    dispatcher_->run_context(
        std::move(startup_handler), std::move(failure_handler), std::move(shutdown_handler));
}

void worker_runtime_context::close() noexcept {
    dispatcher_->close();
}

void worker_runtime_context::stop_timers() noexcept {
    dispatcher_->stop_timers();
}

void worker_runtime_context::detach() noexcept {
    dispatcher_->detach_context();
}
}  // namespace ruvia

namespace ruvia::detail {

worker_stop_chain::worker_stop_chain(worker_runtime_context& context)
    : context_(context),
      work_(asio::make_work_guard(context.io_context())) {}

void worker_stop_chain::configure(hooks_type hooks) noexcept {
    {
        const std::lock_guard lock(mutex_);
        std::swap(hooks_, hooks);
    }
}

void worker_stop_chain::request_stop() noexcept {
    {
        const std::lock_guard lock(mutex_);
        if (stop_requested_) {
            return;
        }
        stop_requested_ = true;
    }
    context_.close();
    try {
        // close() can lose the race to a dispatcher failure that is still
        // notifying listeners. Keep the work guard until all of them have
        // published their cleanup.
        worker_ref_access::when_shutdown_notifications_complete(context_.worker(), [this] {
            const std::lock_guard lock(mutex_);
            defer_stop_control_locked();
        });
    } catch (...) {
        std::terminate();
    }
}

void worker_stop_chain::finalize() noexcept {
    request_stop();
    const std::lock_guard lock(mutex_);
    if (finalization_requested_) {
        return;
    }
    finalization_requested_ = true;
    // A stop control that has not yet run consumes this request itself.
    if (stop_control_ran_ && !work_released_) {
        defer_stop_control_locked();
    }
}

bool worker_stop_chain::post_control(move_only_function<void()> control) noexcept {
    const std::lock_guard lock(mutex_);
    if (work_released_) {
        return false;
    }
    worker_ref_access::defer_or_terminate(context_.worker(), std::move(control));
    return true;
}

void worker_stop_chain::abandon() noexcept {
    std::optional<work_guard_type> work;
    {
        const std::lock_guard lock(mutex_);
        stop_requested_ = true;
        work_released_ = true;
        work.emplace(std::move(work_));
    }
    context_.stop_timers();
}

void worker_stop_chain::defer_stop_control_locked() noexcept {
    worker_ref_access::defer_or_terminate(context_.worker(), [this] { apply_stop(); });
}

void worker_stop_chain::apply_stop() noexcept {
    if (!stop_applied_) {
        stop_applied_ = true;
        try {
            if (hooks_.stop_admission_) {
                hooks_.stop_admission_();
            } else {
                finalize();
            }
        } catch (...) {
            std::terminate();
        }
        context_.stop_timers();
    }
    // Declared before the hook so the work guard is released after it.
    std::optional<work_guard_type> work;
    move_only_function<void()> finalize_hook;
    {
        const std::lock_guard lock(mutex_);
        stop_control_ran_ = true;
        if (!finalization_requested_ || work_released_) {
            return;
        }
        work_released_ = true;
        work.emplace(std::move(work_));
    }
    // Only this finalizer owns the hook after work_released_ is published.
    // Its target's move/destructor may reenter finalize(), so move it unlocked.
    finalize_hook = std::move(hooks_.finalize_);
    try {
        if (finalize_hook) {
            finalize_hook();
        }
    } catch (...) {
        std::terminate();
    }
}

}  // namespace ruvia::detail

namespace ruvia {
worker_group::worker_group() = default;

worker_group::~worker_group() {
    for (auto* member : members_) {
        member->unbind_group(*this);
    }
}

void worker_group::add(worker_runtime& member) {
    const std::lock_guard lock(mutex_);
    if (sealed_ || stopping_) {
        throw std::logic_error("worker group members must be added before start or stop");
    }
    members_.push_back(&member);
    try {
        member.bind_group(*this);
    } catch (...) {
        members_.pop_back();
        throw;
    }
}

void worker_group::launch() {
    {
        const std::lock_guard lock(mutex_);
        if (sealed_) {
            throw std::logic_error("worker group can only be started once");
        }
        sealed_ = true;
    }
    try {
        for (auto* member : members_) {
            if (!launch(*member)) {
                return;
            }
        }
    } catch (...) {
        const auto launch_failure = std::current_exception();
        (void)join(launch_failure);
        std::rethrow_exception(launch_failure);
    }
}

bool worker_group::start() {
    {
        const std::lock_guard lock(mutex_);
        if (sealed_) {
            throw std::logic_error("worker group can only be started once");
        }
        sealed_ = true;
    }
    try {
        for (auto* member : members_) {
            if (!launch(*member)) {
                return settle(false, "ready");
            }
        }
        for (auto* member : members_) {
            if (!settle(member->wait_until_ready(), "ready")) {
                return false;
            }
        }
        const bool reached = settle(true, "ready");
        if (reached) {
            const std::lock_guard lock(mutex_);
            ready_ = true;
        }
        return reached;
    } catch (...) {
        rethrow_phase_failure(std::current_exception());
    }
}

bool worker_group::serve() {
    {
        const std::lock_guard lock(mutex_);
        if (!ready_) {
            throw std::logic_error("worker group must be ready before it serves");
        }
        if (serve_requested_) {
            throw std::logic_error("worker group can only serve once");
        }
        serve_requested_ = true;
    }
    try {
        for (auto* member : members_) {
            if (stop_requested()) {
                return settle(false, "serving");
            }
            member->request_serve();
        }
        for (auto* member : members_) {
            if (!settle(member->wait_until_serving(), "serving")) {
                return false;
            }
        }
        return settle(true, "serving");
    } catch (...) {
        rethrow_phase_failure(std::current_exception());
    }
}

void worker_group::member_failed(worker_runtime& member, std::exception_ptr failure) noexcept {
    {
        const std::lock_guard lock(mutex_);
        if (!failure_) {
            failure_ = std::move(failure);
            failed_member_ = &member;
        }
    }
    request_stop();
}

void worker_group::member_stopping() noexcept {
    {
        const std::lock_guard lock(mutex_);
        if (!stopping_) {
            member_stopped_ = true;
        }
    }
    request_stop();
}

void worker_group::request_stop() noexcept {
    bool dispatch = false;
    {
        const std::lock_guard lock(mutex_);
        if (!stopping_) {
            stopping_ = true;
            stop_dispatching_ = true;
            dispatch = true;
        }
    }
    // Publish group stop before notifying members, which can reenter it.
    // Every caller closes admission on all members; repeats are nonwaiting
    // even while the winning dispatch is notifying an earlier member.
    for (auto* member : members_) {
        member->request_stop();
    }
    if (!dispatch) {
        return;
    }
    const std::lock_guard lock(mutex_);
    stop_dispatching_ = false;
    // Notify before releasing the mutex: a waking join may retire this owner.
    stop_changed_.notify_all();
}

bool worker_group::stop_requested() const noexcept {
    const std::lock_guard lock(mutex_);
    return stopping_;
}

std::exception_ptr worker_group::join(std::exception_ptr primary) noexcept {
    request_stop();
    {
        std::unique_lock lock(mutex_);
        stop_changed_.wait(lock, [this] { return !stop_dispatching_ && !joining_; });
        if (joined_) {
            return join_failure_;
        }
        joining_ = true;
    }
    for (auto* member : members_) {
        try {
            member->join();
        } catch (...) {
            // join() only rejects a member's own thread, which can never
            // own the lifecycle; a stranded owner cannot be recovered.
            std::terminate();
        }
    }
    // The returned failure is settled before any report, so it is never also
    // reported here: the caller's primary, else the first member failure in
    // registration order.
    if (primary == nullptr) {
        for (auto* member : members_) {
            if (auto failure = member->recorded_failure(); failure != nullptr) {
                primary = std::move(failure);
                break;
            }
        }
    }
    // Workers commonly fail for one shared cause (an unreachable dependency),
    // so equivalent failures collapse into a single line with their count.
    // Each member's failure is read once with ownership: it is returned as the
    // primary, collapsed into it, or reported here.
    for (std::size_t index = 0; index != members_.size(); ++index) {
        const auto failure = members_[index]->take_failure();
        if (failure == nullptr || equivalent_failures(failure, primary)) {
            continue;
        }
        bool reported = false;
        for (std::size_t earlier = 0; earlier != index && !reported; ++earlier) {
            reported = equivalent_failures(members_[earlier]->recorded_failure(), failure);
        }
        if (reported) {
            continue;
        }
        std::size_t occurrences = 1;
        for (auto later = index + 1; later != members_.size(); ++later) {
            if (equivalent_failures(members_[later]->recorded_failure(), failure)) {
                ++occurrences;
            }
        }
        report_unhandled_failure("additional worker", failure, occurrences);
    }
    const std::lock_guard lock(mutex_);
    join_failure_ = std::move(primary);
    joining_ = false;
    joined_ = true;
    stop_changed_.notify_all();
    return join_failure_;
}

bool worker_group::launch(worker_runtime& member) {
    {
        const std::lock_guard lock(mutex_);
        if (stopping_) {
            return false;
        }
    }
    // A concurrent stop (group-wide or of this member) can win the runtime's
    // launch transition; start() then reports it instead of launching.
    return member.start();
}

void worker_group::wait_for_stop_dispatch() noexcept {
    std::unique_lock lock(mutex_);
    stop_changed_.wait(lock, [this] { return !stop_dispatching_; });
}

void worker_group::rethrow_phase_failure(std::exception_ptr fallback) {
    request_stop();
    wait_for_stop_dispatch();
    auto failure = observe_phase_failure();
    std::rethrow_exception(failure ? failure : std::move(fallback));
}

std::exception_ptr worker_group::observe_phase_failure() noexcept {
    std::exception_ptr failure;
    worker_runtime* member = nullptr;
    {
        const std::lock_guard lock(mutex_);
        failure = failure_;
        member = failed_member_;
    }
    if (member != nullptr) {
        // A phase waiter may be on a different, healthy member. Observe only
        // the exception actually delivered, preserving other failure reports.
        static_cast<void>(member->take_failure());
    }
    return failure;
}

bool worker_group::settle(bool reached, const char* phase) {
    bool was_stopping = false;
    {
        const std::lock_guard lock(mutex_);
        was_stopping = stopping_;
        if (reached && !stopping_ && !failure_) {
            return true;
        }
    }
    request_stop();
    wait_for_stop_dispatch();
    auto failure = observe_phase_failure();
    bool member_stopped = false;
    {
        const std::lock_guard lock(mutex_);
        member_stopped = member_stopped_;
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (member_stopped || (!reached && !was_stopping)) {
        throw std::runtime_error(std::string("worker stopped before the group was ") + phase);
    }
    return false;
}

}  // namespace ruvia

namespace ruvia {
namespace {

std::size_t default_loop_count() noexcept {
    return std::max<std::size_t>(1, std::thread::hardware_concurrency());
}

}  // namespace

struct event_loop_pool::impl_type final {
    explicit impl_type(event_loop_pool_options options) {
        const auto count = options.loop_count_ == 0 ? default_loop_count() : options.loop_count_;
        if (options.queue_capacity_ == 0) {
            throw std::invalid_argument("event loop queue capacity must be greater than zero");
        }
        loops_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            loops_.push_back(std::make_shared<detail::event_loop_state>(
                options.queue_capacity_, options.blocking_pool_));
            loops_.back()->install_shutdown_registration();
        }
    }

    void bind_callbacks(const std::shared_ptr<impl_type>& self) {
        const std::weak_ptr<impl_type> weak_owner = self;
        for (const auto& loop : loops_) {
            const std::weak_ptr<detail::event_loop_state> weak_loop = loop;
            loop->failure_sink_ = [weak_owner, weak_loop](std::exception_ptr failure) noexcept {
                if (const auto owner = weak_owner.lock()) {
                    if (const auto locked_loop = weak_loop.lock()) {
                        owner->report_failure(*locked_loop, std::move(failure));
                        return;
                    }
                }
                report_unhandled_failure("event loop failure after pool retirement", std::move(failure));
            };
            auto* const stable_runtime = &loop->owned_runtime();
            stable_runtime->configure({
                .stop_admission_ = [weak_loop, stable_runtime] {
                    if (const auto locked_loop = weak_loop.lock()) {
                        locked_loop->complete_stop_admission();
                    } else {
                        // Construction rollback can destroy an unpublished
                        // state before its core owner drains this control.
                        stable_runtime->finalize();
                    } },
            });
            group_.add(*stable_runtime);
        }
    }

    void report_failure(detail::event_loop_state& loop, std::exception_ptr failure) noexcept {
        if (!failure) {
            return;
        }
        bool joined = false;
        {
            const std::lock_guard lock(failure_mutex_);
            joined = joined_;
        }
        if (joined) {
            report_unhandled_failure("event loop failure after pool retirement", std::move(failure));
            return;
        }
        if (!loop.owned_runtime().fail(failure)) {
            if (!equivalent_failures(failure, loop.recorded_failure())) {
                report_unhandled_failure("additional event loop failure", failure);
            }
        }
        stop();
    }

    void start() {
        group_.launch();
    }

    void stop() noexcept {
        group_.request_stop();
        for (const auto& loop : loops_) {
            loop->stop();
        }
    }

    [[nodiscard]] std::exception_ptr join() noexcept {
        auto failure = group_.join();
        for (const auto& loop : loops_) {
            detail::worker_ref_access::wait_for_reservations(loop->runtime_.worker());
        }
        const std::lock_guard lock(failure_mutex_);
        if (joined_) {
            return nullptr;
        }
        joined_ = true;
        return failure;
    }

    std::vector<std::shared_ptr<detail::event_loop_state>> loops_;
    worker_group group_;
    std::mutex failure_mutex_;
    bool joined_{false};
    std::atomic<std::size_t> next_index_{0};
};

event_loop_pool::event_loop_pool(event_loop_pool_options options)
    : impl_(std::make_shared<impl_type>(options)) {
    impl_->bind_callbacks(impl_);
}

event_loop_pool::~event_loop_pool() {
    if (std::ranges::any_of(
            impl_->loops_, [](const auto& loop) { return loop->runtime_.worker().is_current(); })) {
        std::terminate();
    }
    impl_->stop();
    if (const auto failure = impl_->join()) {
        // Destroying a pool that was never joined explicitly makes this the
        // only place its first failure is ever observed, and a destructor
        // cannot rethrow it further. Report rather than end here.
        report_unhandled_failure("event loop pool", failure);
    }
}

void event_loop_pool::start() {
    impl_->start();
}

void event_loop_pool::stop() noexcept {
    impl_->stop();
}

void event_loop_pool::join() {
    if (std::ranges::any_of(
            impl_->loops_, [](const auto& loop) { return loop->runtime_.worker().is_current(); })) {
        throw std::logic_error("cannot join an event loop pool from one of its workers");
    }
    impl_->stop();
    if (const auto failure = impl_->join()) {
        std::rethrow_exception(failure);
    }
}

std::size_t event_loop_pool::loop_count() const noexcept {
    return impl_->loops_.size();
}

event_loop event_loop_pool::loop(std::size_t index) const {
    if (index >= impl_->loops_.size()) {
        throw std::out_of_range("event loop index is out of range");
    }
    return event_loop(impl_->loops_[index]);
}

event_loop event_loop_pool::next_loop() noexcept {
    const auto index = impl_->next_index_.fetch_add(1, std::memory_order_relaxed);
    return event_loop(impl_->loops_[index % impl_->loops_.size()]);
}

event_loop event_loop_pool::loop_for(std::uint64_t key) const noexcept {
    return event_loop(impl_->loops_[key % impl_->loops_.size()]);
}

event_loop event_loop_pool::loop_for(std::string_view key) const noexcept {
    return loop_for(worker_selection_hash(key));
}

}  // namespace ruvia

namespace ruvia {
namespace {

// Routes through the loop while it is alive, or to the unhandled-failure
// diagnostic under `context` once it has retired.
void report_through_loop(const std::weak_ptr<detail::event_loop_state>& loop, std::exception_ptr failure,
    const char* context) noexcept {
    if (const auto state_value = loop.lock()) {
        state_value->route_failure(std::move(failure), context);
        return;
    }
    report_unhandled_failure(context, std::move(failure));
}

}  // namespace

namespace detail {

class external_context_attachment_service final : public asio::execution_context::service {
public:
    static asio::execution_context::id id;

    explicit external_context_attachment_service(asio::execution_context& context_value)
        : asio::execution_context::service(context_value) {}

    [[nodiscard]] bool claim() noexcept {
        const std::lock_guard lock(mutex_);
        if (claimed_) {
            return false;
        }
        claimed_ = true;
        return true;
    }

    void release_claim() noexcept {
        const std::lock_guard lock(mutex_);
        claimed_ = false;
    }

    void retain(std::shared_ptr<event_loop_state> state_value) noexcept {
        {
            const std::lock_guard lock(mutex_);
            state_.swap(state_value);
        }
    }

    void release_state(event_loop_state* state_value) noexcept {
        std::shared_ptr<event_loop_state> abandoned;
        {
            const std::lock_guard lock(mutex_);
            if (state_.get() != state_value) {
                return;
            }
            abandoned = std::move(state_);
            claimed_ = false;
        }
        abandoned.reset();
    }

private:
    void shutdown() override {
        std::shared_ptr<event_loop_state> state;
        {
            const std::lock_guard lock(mutex_);
            state = std::move(state_);
            claimed_ = false;
        }
        if (state) {
            // The context is entering Asio's service shutdown, so no new handler
            // may be queued here. Retire the worker before the context destroys
            // its scheduler. The state may still be held by event_loop/Attachment;
            // those handles become terminal and no longer expose a dangling
            // io_context reference.
            state->shutdown_external_context();
        }
    }

    std::mutex mutex_;
    bool claimed_{false};
    std::shared_ptr<event_loop_state> state_;
};

asio::execution_context::id external_context_attachment_service::id;

external_context_claim::external_context_claim(asio::io_context& io_context)
    : service_(&asio::use_service<external_context_attachment_service>(io_context)) {
    if (!service_->claim()) {
        throw std::invalid_argument("an io_context can have only one Ruvia event loop attachment");
    }
}

external_context_claim::~external_context_claim() {
    if (!retained_) {
        service_->release_claim();
    }
}

void external_context_claim::retain(std::shared_ptr<event_loop_state> state_value) noexcept {
    service_->retain(std::move(state_value));
    retained_ = true;
}

void external_context_claim::release(event_loop_state* state_value) noexcept {
    service_->release_state(state_value);
}

attached_context::attached_context(asio::io_context& io_context, std::size_t queue_capacity, blocking_pool* pool,
    move_only_function<void(std::exception_ptr)> failure)
    : claim_(io_context),
      runtime_(io_context, queue_capacity, pool),
      stop_(runtime_),
      failure_(std::move(failure)) {
    worker_ref_access::enable_external_runner(runtime_.worker());
}

// The callback owner is kept alive by the completion handler until both the
// returned task frame and its completion chain have retired.
class event_loop_stop_listener final : public std::enable_shared_from_this<event_loop_stop_listener> {
public:
    event_loop_stop_listener(worker_ref worker_value, asio::io_context::executor_type executor,
        move_only_function<task<void>()> callback_value, std::weak_ptr<event_loop_state> loop)
        : worker_(std::move(worker_value)),
          executor_(std::move(executor)),
          callback_(std::move(callback_value)),
          loop_(std::move(loop)) {}

    void install_shutdown_registration() {
        if (worker_.register_shutdown(shared_from_this(), &event_loop_stop_listener::shutdown_registration_) !=
            worker_registration_status::registered) {
            throw std::runtime_error("cannot register a stop callback on a stopping event loop");
        }
    }

    void cancel() noexcept {
        move_only_function<task<void>()> abandoned;
        std::shared_ptr<event_loop_state> active_loop;
        {
            const std::lock_guard lock(mutex_);
            if (started_ || cancelled_) {
                return;
            }
            cancelled_ = true;
            if (std::exchange(retirement_begun_, false)) {
                active_loop = std::move(active_loop_);
            }
        }
        // Cancellation owns the callback after publishing cancelled_; neither
        // the stop notification nor a deferred start may use it now.
        abandoned = std::move(callback_);
        abandoned = nullptr;
        if (active_loop) {
            active_loop->retirement_.finish();
        }
    }

    void worker_stopping() noexcept {
        auto loop = loop_.lock();
        if (!loop) {
            return;
        }
        {
            const std::lock_guard lock(mutex_);
            if (cancelled_ || !callback_ || retirement_begun_ || started_) {
                return;
            }
            loop->retirement_.begin();
            active_loop_ = std::move(loop);
            retirement_begun_ = true;
        }
        auto self = shared_from_this();
        if (worker_.is_current()) {
            start_callback(std::move(self));
            return;
        }
        worker_ref_access::defer_or_terminate(worker_,
            [self = std::move(self)]() mutable noexcept { self->start_callback(std::move(self)); });
    }

private:
    void report(std::exception_ptr failure) const noexcept {
        report_through_loop(loop_, std::move(failure), "event loop stop callback");
    }

    void start_callback(std::shared_ptr<event_loop_stop_listener> self) noexcept {
        {
            const std::lock_guard lock(mutex_);
            if (cancelled_ || !callback_ || !retirement_begun_) {
                return;
            }
            started_ = true;
        }
        std::optional<task<void>> task;
        try {
            task.emplace(callback_());
        } catch (const std::bad_alloc&) {
            report(std::current_exception());
            std::terminate();
        } catch (...) {
            report(std::current_exception());
            callback_ = nullptr;
            auto active_loop = std::move(active_loop_);
            active_loop->retirement_.finish();
            return;
        }

        try {
            async_start(std::move(*task),
                asio::bind_executor(executor_,
                    [self = std::move(self)](std::exception_ptr failure) mutable {
                        if (failure != nullptr) {
                            self->report(std::move(failure));
                        }
                        self->callback_ = nullptr;
                        auto active_loop = std::move(self->active_loop_);
                        active_loop->retirement_.finish();
                    }));
        } catch (...) {
            report(std::current_exception());
            std::terminate();
        }
    }

    // First member: unlinks from the worker registry after every other member is gone.
    worker_shutdown_registration shutdown_registration_;
    worker_ref worker_;
    asio::io_context::executor_type executor_;
    std::mutex mutex_;
    bool retirement_begun_{false};
    bool cancelled_{false};
    bool started_{false};
    move_only_function<task<void>()> callback_;
    std::weak_ptr<event_loop_state> loop_;
    std::shared_ptr<event_loop_state> active_loop_;
};

event_loop_state::event_loop_state(std::size_t queue_capacity, blocking_pool* pool)
    : context_ownership_(std::in_place_type<worker_runtime>,
          worker_runtime_options{.queue_capacity_ = queue_capacity, .blocking_pool_ = pool}),
      io_context_(&owned_runtime().io_context()),
      executor_(io_context_->get_executor()),
      runtime_(owned_runtime().context()) {}

event_loop_state::event_loop_state(worker_runtime& runtime)
    : context_ownership_(std::in_place_type<borrowed_runtime>, &runtime),
      io_context_(&runtime.io_context()),
      executor_(io_context_->get_executor()),
      runtime_(runtime.context()) {}

event_loop_state::event_loop_state(asio::io_context& external_context, std::size_t queue_capacity, blocking_pool* pool,
    move_only_function<void(std::exception_ptr)> failure)
    : context_ownership_(std::in_place_type<attached_context>, external_context, queue_capacity, pool, std::move(failure)),
      io_context_(std::addressof(external_context)),
      executor_(external_context.get_executor()),
      runtime_(std::get_if<attached_context>(&context_ownership_)->runtime_) {}

void event_loop_state::attach(const std::shared_ptr<event_loop_state>& self) {
    auto& attached = *std::get_if<attached_context>(&context_ownership_);
    attached.stop_.configure({
        // Only stop() requests this chain, so its gate is already sealing.
        // Joining it keeps the chain from finalizing before the gate drains.
        .stop_admission_ = [this] { complete_stop_admission(); },
        // Releasing the claim can destroy this state together with its chain;
        // the chain then releases its work guard on the still-running context.
        .finalize_ =
            [this, &attached] {
                runtime_.detach();
                attached.claim_.release(this);
            },
    });
    attached.claim_.retain(self);
}

void event_loop_state::shutdown_external_context() noexcept {
    // Only attach() retains a state with this service, so it is an attached loop.
    auto& attached = *std::get_if<attached_context>(&context_ownership_);
    retirement_.close_admission();
    if (retirement_.pending() != 0) {
        // Their frames and leases are owned by handlers that this shutdown
        // destroys without running; nothing can retire them safely.
        report_unhandled_failure("attached event loop",
            std::make_exception_ptr(std::logic_error(
                "io_context destroyed while its attached event loop still had root tasks, stop "
                "callbacks, or cleanup pending; keep running the context until the attachment retires")));
        std::terminate();
    }
    stopping_.store(true, std::memory_order_release);
    attached.stop_.abandon();
    runtime_.detach();
    worker_ref_access::wait_for_reservations(runtime_.worker());
}
void event_loop_state::install_shutdown_registration() {
    if (runtime_.worker().register_shutdown(shared_from_this(), &event_loop_state::shutdown_registration_) !=
        worker_registration_status::registered) {
        throw std::runtime_error("cannot register an event loop on a stopping worker");
    }
}

void event_loop_state::worker_stopping_complete() noexcept {
    stop();
}

void event_loop_state::route_failure(std::exception_ptr failure, const char* unbound_context) noexcept {
    if (failure_sink_) {
        failure_sink_(std::move(failure));
        return;
    }
    report_unhandled_failure(unbound_context, std::move(failure));
}

void event_loop_state::report_attached_failure(std::exception_ptr failure) noexcept {
    stop();
    auto& receiver = std::get_if<attached_context>(&context_ownership_)->failure_;
    if (!receiver) {
        report_unhandled_failure("attached event loop failure", std::move(failure));
        return;
    }
    try {
        receiver(std::move(failure));
    } catch (...) {
        report_unhandled_failure("attached event loop failure callback", std::current_exception());
    }
}

void event_loop_state::fail_teardown_start(std::exception_ptr failure) noexcept {
    route_failure(std::move(failure), "event loop teardown startup");
    std::terminate();
}

void event_loop_state::stop() noexcept {
    if (stopping_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    const std::weak_ptr<event_loop_state> weak = weak_from_this();
    if (!retirement_.begin_stop([weak] {
            if (auto state_value = weak.lock()) {
                auto* const state_ptr = state_value.get();
                state_ptr->defer_finalize_stop(std::move(state_value));
            }
        })) {
        return;
    }
    if (auto* attached = std::get_if<attached_context>(&context_ownership_)) {
        attached->stop_.request_stop();
    } else if (auto* borrowed = std::get_if<borrowed_runtime>(&context_ownership_)) {
        borrowed->runtime_->request_stop();
    } else {
        owned_runtime().request_stop();
    }
}

void event_loop_state::complete_stop_admission() noexcept {
    stop();
    schedule_finalize_when_idle();
}

void event_loop_state::schedule_finalize_when_idle() noexcept {
    if (!runtime_.worker().valid()) {
        return;
    }
    const std::weak_ptr<event_loop_state> weak = weak_from_this();
    try {
        worker_ref_access::when_idle(runtime_.worker(), [weak] {
            if (const auto idle_state = weak.lock()) {
                idle_state->retirement_.seal();
            }
        });
    } catch (...) {
        if (!runtime_.worker().valid()) {
            return;
        }
        fail_teardown_start(std::current_exception());
    }
}

void event_loop_state::defer_finalize_stop(std::shared_ptr<event_loop_state> self) noexcept {
    const auto worker_value = runtime_.worker();
    try {
        // A detached context ran shutdown_external_context(), which
        // already abandoned the stop chain this would finalize.
        (void)worker_value.defer_completion([state_value = std::move(self)] { state_value->finalize_stop(); });
    } catch (...) {
        fail_teardown_start(std::current_exception());
    }
}

void event_loop_state::finalize_stop() noexcept {
    if (auto* attached = std::get_if<attached_context>(&context_ownership_)) {
        attached->stop_.finalize();
    } else if (auto* borrowed = std::get_if<borrowed_runtime>(&context_ownership_)) {
        borrowed->runtime_->finish_loop_stop_admission();
    } else {
        owned_runtime().finalize();
    }
}

void report_root_task_failure(const std::weak_ptr<event_loop_state>& loop, std::exception_ptr failure) noexcept {
    report_through_loop(loop, std::move(failure), "unobserved event-loop root task");
}

}  // namespace detail

event_loop_stop_registration::event_loop_stop_registration(
    std::shared_ptr<detail::event_loop_stop_listener> listener_value) noexcept
    : listener_(std::move(listener_value)) {}

event_loop_start_rejected::event_loop_start_rejected(post_status status)
    : std::runtime_error(status == post_status::queue_full ? "event loop root task queue is full"
                                                           : "event loop is stopping"),
      status_(status) {}

event_loop_stop_registration::~event_loop_stop_registration() {
    reset();
}

event_loop_stop_registration& event_loop_stop_registration::operator=(
    event_loop_stop_registration&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    // Commit the incoming listener before retiring a callback that may own
    // the source registration or reenter this destination.
    event_loop_stop_registration retired(std::move(other));
    listener_.swap(retired.listener_);
    return *this;
}

bool event_loop_stop_registration::valid() const noexcept {
    return listener_ != nullptr;
}

void event_loop_stop_registration::reset() noexcept {
    if (auto listener = std::move(listener_)) {
        listener->cancel();
    }
}

event_loop::event_loop(std::shared_ptr<detail::event_loop_state> state_value) noexcept
    : state_(std::move(state_value)) {}

bool event_loop::valid() const noexcept {
    return worker().valid();
}

asio::io_context& event_loop::io_context() const& {
    if (!state_ || !state_->runtime_.worker().valid()) {
        if (state_) {
            throw std::logic_error("event loop execution context is detached");
        }
        throw std::logic_error("cannot access a default-constructed event loop");
    }
    return *state_->io_context_;
}

asio::io_context::executor_type event_loop::executor() const {
    static_cast<void>(io_context());
    return state_->executor_;
}

const worker_ref& event_loop::worker() const& noexcept {
    if (state_) {
        return state_->runtime_.worker();
    }
    static const worker_ref empty_worker;
    return empty_worker;
}

worker_ref event_loop::worker() const&& noexcept {
    return worker();
}

void event_loop::report_failure(std::exception_ptr failure) const noexcept {
    if (!failure) {
        return;
    }
    if (const auto state_value = state_) {
        state_value->route_failure(std::move(failure), "event loop failure");
    } else {
        report_unhandled_failure("invalid event loop failure", std::move(failure));
    }
}

std::shared_ptr<void> event_loop::acquire_root_lease() const {
    if (!state_) {
        return {};
    }
    return state_->retirement_.try_acquire(state_);
}

bool event_loop::defer_cleanup_task(move_only_function<void()> task_value) const {
    if (!task_value) {
        throw std::invalid_argument("event loop cleanup must be callable");
    }
    if (!state_ || !state_->retirement_.try_begin_cleanup()) {
        return false;
    }
    struct cleanup_control final {
        // Destroy inputs and finish the retirement gate before releasing the
        // context owner. Do not rely on lambda capture member destruction order.
        std::shared_ptr<detail::event_loop_state> state_;
        move_only_function<void()> task_;

        cleanup_control(std::shared_ptr<detail::event_loop_state> state_value,
            move_only_function<void()> task_input) noexcept
            : state_(std::move(state_value)),
              task_(std::move(task_input)) {}
        cleanup_control(const cleanup_control&) = delete;
        cleanup_control& operator=(const cleanup_control&) = delete;
        cleanup_control(cleanup_control&& other) noexcept
            : state_(std::move(other.state_)),
              task_(std::move(other.task_)) {}
        cleanup_control& operator=(cleanup_control&&) = delete;
        ~cleanup_control() {
            if (state_) {
                task_ = nullptr;
                state_->retirement_.finish();
            }
        }
    };
    return state_->runtime_.worker().defer_completion([control = cleanup_control(state_, std::move(task_value))]() mutable { control.task_(); });
}

event_loop_stop_registration event_loop::register_stop_callback(
    move_only_function<task<void>()> callback_value) const {
    if (!state_) {
        throw std::logic_error("cannot register a stop callback on an invalid event loop");
    }
    if (!callback_value) {
        throw std::invalid_argument("event loop stop callback must be callable");
    }
    if (!state_->runtime_.worker().accepting()) {
        throw std::runtime_error("cannot register a stop callback on a stopping event loop");
    }
    auto listener_value = std::make_shared<detail::event_loop_stop_listener>(
        state_->runtime_.worker(), state_->executor_, std::move(callback_value), state_);
    listener_value->install_shutdown_registration();
    return event_loop_stop_registration(std::move(listener_value));
}

event_loop_attachment::event_loop_attachment(std::shared_ptr<detail::event_loop_state> state_value) noexcept
    : state_(std::move(state_value)) {}

event_loop_attachment::~event_loop_attachment() {
    stop();
}

event_loop_attachment::event_loop_attachment(event_loop_attachment&& other) noexcept
    : state_(std::move(other.state_)) {}

bool event_loop_attachment::valid() const noexcept {
    return state_ != nullptr && state_->runtime_.worker().valid();
}

event_loop event_loop_attachment::loop() const noexcept {
    return event_loop(state_);
}

void event_loop_attachment::run() {
    auto state_value = state_;
    if (!state_value) {
        throw std::logic_error("cannot run an invalid event loop attachment");
    }
    if (!state_value->runtime_.worker().valid()) {
        throw std::logic_error("event loop execution context is detached");
    }
    state_value->runtime_.run();
}

void event_loop_attachment::stop() noexcept {
    if (state_) {
        state_->stop();
    }
}

event_loop_attachment attach_event_loop(
    asio::io_context& io_context, event_loop_attachment_options options) {
    auto state_value = std::make_shared<detail::event_loop_state>(
        io_context, options.queue_capacity_, options.blocking_pool_, std::move(options.failure_));
    const std::weak_ptr<detail::event_loop_state> weak_state = state_value;
    // root_task uses this sink directly; it must not call event_loop_state::route_failure,
    // which would route back through this same sink.
    state_value->failure_sink_ = [weak_state](std::exception_ptr failure) {
        if (const auto locked_state = weak_state.lock()) {
            locked_state->report_attached_failure(std::move(failure));
        } else {
            report_unhandled_failure("attached event loop failure after retirement", std::move(failure));
        }
    };
    state_value->install_shutdown_registration();
    state_value->attach(state_value);
    return event_loop_attachment(std::move(state_value));
}

}  // namespace ruvia
