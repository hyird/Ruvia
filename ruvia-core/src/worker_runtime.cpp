#include "ruvia/core/worker_runtime.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

#include <asio/config.hpp>
#if defined(_WIN32)
#include <windows.h>
#endif

#include "ruvia/core/event_loop.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/worker_group.h"

#include "event_loop_state.h"
#include "worker_dispatcher_impl.h"
#include "worker_runtime_context.h"
#include "worker_stop_chain.h"

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

    // 上下文跨越启动、运行和析构线程，使用可跨线程释放的进程级资源。
    explicit impl_type(worker_runtime_options options)
        : io_context_(std::allocator_arg, std::pmr::polymorphic_allocator<std::byte>(process_resource()),
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
        auto created = std::allocate_shared<detail::event_loop_state>(
            std::pmr::polymorphic_allocator<detail::event_loop_state>(process_resource()), owner);
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
        if (context_.handle().register_shutdown(created, &detail::event_loop_state::shutdown_registration_) !=
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
        if (context_.handle().is_current()) {
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
        if (context_.handle().is_current()) {
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

void worker_runtime::impl_deleter_type::operator()(impl_type* value) const noexcept {
    destroy_pmr_object(value, process_resource());
}

worker_runtime::worker_runtime(worker_runtime_options options)
    : impl_(construct_pmr_object<impl_type>(process_resource(), options)) {}

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
worker_runtime::thread_join_operation::thread_join_operation(worker_runtime& owner, worker_handle caller) noexcept
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
    if (owner_->handle().is_current()) {
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

worker_runtime::thread_join_operation worker_runtime::prepare_thread_join(worker_handle caller) & {
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

const worker_handle& worker_runtime::handle() const& noexcept {
    return impl_->context_.handle();
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
      dispatcher_(std::allocate_shared<detail::worker_dispatcher>(
          std::pmr::polymorphic_allocator<detail::worker_dispatcher>(process_resource()),
          io_context, queue_capacity, pool)),
      handle_(detail::worker_handle_access::make(dispatcher_)) {}

worker_runtime_context::~worker_runtime_context() {
    detach();
}

asio::io_context& worker_runtime_context::io_context() const& noexcept {
    return *io_context_;
}

const worker_handle& worker_runtime_context::handle() const& noexcept {
    return handle_;
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
