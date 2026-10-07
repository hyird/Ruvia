#include "ruvia/core/worker_runtime.h"

#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include <asio/executor_work_guard.hpp>

#include "ruvia/core/detail/util/FailureReport.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/core/memory/ProcessResource.h"

namespace ruvia {

class worker_runtime::impl final {
public:
    explicit impl(worker_runtime_options options)
        : io_context_(options.io_policy == worker_io_policy::single_owner
                          ? ASIO_CONCURRENCY_HINT_UNSAFE_IO
                          : ASIO_CONCURRENCY_HINT_DEFAULT),
          work_(asio::make_work_guard(io_context_)),
          context_(io_context_, options.mailbox_capacity) {}

    void record_failure(std::exception_ptr failure, bool observed = false) noexcept {
        const std::lock_guard lock(mutex_);
        if (failure_ == nullptr) {
            failure_ = std::move(failure);
            failure_observed_ = observed;
        }
    }

    [[nodiscard]] std::exception_ptr unobserved_failure() noexcept {
        const std::lock_guard lock(mutex_);
        if (failure_observed_) {
            return {};
        }
        failure_observed_ = true;
        return failure_;
    }

    void configure(worker_runtime_hooks hooks) {
        const std::lock_guard lock(mutex_);
        if (configured_ || lifecycle_.state() != RuntimeLifecycle::State::kReady) {
            throw std::logic_error("worker runtime hooks must be configured once before launch");
        }
        hooks_ = std::move(hooks);
        configured_ = true;
    }

    void start() {
        std::unique_lock lock(mutex_);
        if (!lifecycle_.start()) {
            throw std::logic_error("worker runtime can only be started once");
        }
        try {
            thread_ = std::thread([this] { run(true); });
            started_ = true;
        } catch (...) {
            const auto failure = std::current_exception();
            lock.unlock();
            record_failure(failure, true);
            request_stop();
            std::rethrow_exception(failure);
        }
    }

    void request_stop() noexcept {
        if (!lifecycle_.requestStop()) {
            return;
        }
        context_.close();
        try {
            // A racing dispatcher failure may still be notifying listeners.
            // Keep the guard until all of them have published their cleanup.
            detail::WorkerHandleAccess::whenShutdownNotificationsComplete(context_.handle(),
                [this] {
                    const std::lock_guard lock(mutex_);
                    stop_control_scheduled_ = true;
                    context_.deferOrTerminate([this] { apply_stop(); });
                });
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] bool post_control(MoveOnlyFunction<void()> control) noexcept {
        const std::lock_guard lock(mutex_);
        if (lifecycle_.state() != RuntimeLifecycle::State::kRunning || work_released_) {
            return false;
        }
        context_.deferOrTerminate(std::move(control));
        return true;
    }

    void finalize(MoveOnlyFunction<void()> cleanup) noexcept {
        {
            const std::lock_guard lock(mutex_);
            if (finalization_requested_) {
                return;
            }
            finalization_requested_ = true;
            cleanup_ = std::move(cleanup);
        }
        request_stop();
        const std::lock_guard lock(mutex_);
        // If request_stop is still notifying listeners, its eventual control
        // consumes this request. Never release work before that publication.
        if (stop_control_scheduled_ && !work_released_) {
            context_.deferOrTerminate([this] { apply_stop(); });
        }
    }

    void apply_stop() noexcept {
        if (!stop_applied_) {
            stop_applied_ = true;
            try {
                if (hooks_.stop_admission) {
                    hooks_.stop_admission();
                } else {
                    finalize({});
                }
            } catch (...) {
                std::terminate();
            }
            context_.stopTimers();
        }
        MoveOnlyFunction<void()> cleanup;
        {
            const std::lock_guard lock(mutex_);
            if (!finalization_requested_ || work_released_) {
                return;
            }
            work_released_ = true;
            cleanup = std::move(cleanup_);
        }
        try {
            if (cleanup) {
                cleanup();
            }
        } catch (...) {
            std::terminate();
        }
        work_.reset();
    }

    void run(bool launch_startup) noexcept {
        try {
            context_.run(
                [this, launch_startup] {
                    if (launch_startup && hooks_.startup) {
                        hooks_.startup();
                    }
                },
                [this](std::exception_ptr failure) noexcept {
                    record_failure(failure, static_cast<bool>(hooks_.failure));
                    request_stop();
                    try {
                        if (hooks_.failure) {
                            hooks_.failure(std::move(failure));
                        }
                    } catch (...) {
                        std::terminate();
                    }
                },
                [this] {
                    if (hooks_.shutdown) {
                        hooks_.shutdown();
                    }
                });
        } catch (...) {
            record_failure(std::current_exception());
        }
        // Detach before the context or domain owners can disappear. Unfinished
        // post factories own their stable endpoint, not this execution context.
        context_.detach();
        work_.reset();
        (void)lifecycle_.requestStop();
        lifecycle_.completeStop();
    }

    void join() {
        if (context_.handle().isCurrent()) {
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
            condition_.wait(lock, [this] { return !joining_; });
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
                record_failure(std::current_exception());
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
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    WorkerRuntimeContext context_;
    RuntimeLifecycle lifecycle_;
    worker_runtime_hooks hooks_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::thread thread_;
    std::exception_ptr failure_;
    bool failure_observed_{};
    MoveOnlyFunction<void()> cleanup_;
    bool configured_{};
    bool started_{};
    bool joining_{};
    bool joined_{};
    bool stop_control_scheduled_{};
    bool finalization_requested_{};
    bool work_released_{};
    bool stop_applied_{};
};

void worker_runtime::impl_deleter::operator()(impl* value) const noexcept {
    detail::destroyPmrObject(value, detail::processResource());
}

worker_runtime::worker_runtime(worker_runtime_options options)
    : impl_(detail::constructPmrObject<impl>(detail::processResource(), options)) {}

worker_runtime::~worker_runtime() {
    impl_->request_stop();
    try {
        impl_->join();
    } catch (...) {
        // An owner cannot safely detach a running thread to recover self-join.
        std::terminate();
    }
    if (const auto failure = impl_->unobserved_failure()) {
        detail::reportUnhandledFailure("worker runtime", failure);
    }
}

void worker_runtime::configure(worker_runtime_hooks hooks) {
    impl_->configure(std::move(hooks));
}

void worker_runtime::start() {
    impl_->start();
}

void worker_runtime::request_stop() noexcept {
    impl_->request_stop();
}

bool worker_runtime::post_control(MoveOnlyFunction<void()> control) noexcept {
    return impl_->post_control(std::move(control));
}

void worker_runtime::finalize(MoveOnlyFunction<void()> cleanup) noexcept {
    impl_->finalize(std::move(cleanup));
}

void worker_runtime::join() {
    impl_->join();
}

std::exception_ptr worker_runtime::failure() const noexcept {
    const std::lock_guard lock(impl_->mutex_);
    impl_->failure_observed_ = true;
    return impl_->failure_;
}

void worker_runtime::rethrow_failure() const {
    if (const auto error = failure()) {
        std::rethrow_exception(error);
    }
}

RuntimeLifecycle::State worker_runtime::state() const noexcept {
    return impl_->lifecycle_.state();
}

bool worker_runtime::started() const noexcept {
    const std::lock_guard lock(impl_->mutex_);
    return impl_->started_;
}

WorkerRuntimeContext& worker_runtime::context() & noexcept {
    return impl_->context_;
}

}  // namespace ruvia
