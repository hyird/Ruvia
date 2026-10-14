#include "ruvia/core/worker_runtime.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "ruvia/core/failure_report.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/memory/process_resource.h"
#include "ruvia/core/worker_group.h"

#include "first_failure.h"
#include "worker_stop_chain.h"

namespace ruvia {

class worker_runtime::impl final {
public:
    enum class phase_type : std::uint8_t { pending,
        reached,
        aborted };

    explicit impl(worker_runtime_options options)
        : io_context_(options.io_policy_ == worker_io_policy::single_owner
                          ? ASIO_CONCURRENCY_HINT_UNSAFE_IO
                          : ASIO_CONCURRENCY_HINT_DEFAULT),
          context_(io_context_, options.queue_capacity_),
          stop_(context_) {}

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
            lifecycle_.state() != runtime_lifecycle::state_type::ready) {
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
        const std::lock_guard lock(mutex_);
        if (configured_ || lifecycle_.state() != runtime_lifecycle::state_type::ready) {
            throw std::logic_error("worker runtime hooks must be configured once before launch");
        }
        stop_.configure({
            .stop_admission_ = std::move(hooks.stop_admission_),
            .finalize_ = std::move(hooks.finalize_),
        });
        hooks_ = std::move(hooks);
        configured_ = true;
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
                lifecycle_.state() != runtime_lifecycle::state_type::running) {
                return;
            }
            ready_ = phase_type::reached;
        }
        phase_changed_.notify_all();
    }

    [[nodiscard]] bool wait_for(const phase_type& phase) {
        std::unique_lock lock(mutex_);
        if (lifecycle_.state() == runtime_lifecycle::state_type::ready) {
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
        if (lifecycle_.state() != runtime_lifecycle::state_type::running) {
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
                lifecycle_.state() != runtime_lifecycle::state_type::running) {
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
        const std::lock_guard lock(mutex_);
        stop_dispatching_ = false;
        stop_dispatch_thread_ = {};
        condition_.notify_all();
    }

    [[nodiscard]] bool post_control(move_only_function<void()> control) noexcept {
        if (lifecycle_.state() != runtime_lifecycle::state_type::running) {
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

    void join() {
        if (context_.handle().is_current()) {
            throw std::logic_error("cannot join a worker runtime from its owner");
        }
        request_stop();
        std::thread joining_thread;
        bool drain_unstarted = false;
        {
            std::unique_lock lock(mutex_);
            if (thread_.joinable() && thread_.get_id() == std::this_thread::get_id()) {
                throw std::logic_error("cannot join a worker runtime from its owner");
            }
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
    bool joining_{};
    bool joined_{};
    worker_group* group_{nullptr};
    worker_runtime* group_member_{nullptr};
    std::size_t group_notifications_{0};
};

void worker_runtime::impl_deleter::operator()(impl* value) const noexcept {
    destroy_pmr_object(value, process_resource());
}

worker_runtime::worker_runtime(worker_runtime_options options)
    : impl_(construct_pmr_object<impl>(process_resource(), options)) {}

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

std::exception_ptr worker_runtime::failure() const noexcept {
    const std::lock_guard lock(impl_->mutex_);
    return impl_->failure_.observe();
}

runtime_lifecycle::state_type worker_runtime::state() const noexcept {
    return impl_->lifecycle_.state();
}

worker_runtime_context& worker_runtime::context() & noexcept {
    return impl_->context_;
}

}  // namespace ruvia
