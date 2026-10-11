#include "ruvia/core/worker_runtime.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include <asio/executor_work_guard.hpp>

#include "ruvia/core/detail/worker/worker_dispatcher.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/memory/process_resource.h"

#include "first_failure.h"

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
          work_(asio::make_work_guard(io_context_)),
          context_(io_context_, options.queue_capacity_) {}

    // Returns true for the winning failure.
    [[nodiscard]] bool record_failure(std::exception_ptr failure, bool observed) noexcept {
        {
            const std::lock_guard lock(mutex_);
            if (!failure_.record(std::move(failure), observed)) {
                return false;
            }
        }
        phase_changed_.notify_all();
        return true;
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
            (void)record_failure(failure, true);
            request_stop();
            std::rethrow_exception(failure);
        }
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

    // Returns whether the owner thread had launched when phases closed.
    bool abort_phases() noexcept {
        bool launched = false;
        {
            const std::lock_guard lock(mutex_);
            launched = started_;
            if (ready_ == phase_type::pending) {
                ready_ = phase_type::aborted;
            }
            if (serving_ == phase_type::pending) {
                serving_ = phase_type::aborted;
            }
        }
        phase_changed_.notify_all();
        return launched;
    }

    void request_stop() noexcept {
        if (!lifecycle_.request_stop()) {
            return;
        }
        if (!abort_phases() && hooks_.abandon_) {
            try {
                hooks_.abandon_();
            } catch (...) {
                std::terminate();
            }
        }
        context_.close();
        try {
            // A racing dispatcher failure may still be notifying listeners.
            // Keep the guard until all of them have published their cleanup.
            detail::worker_handle_access::when_shutdown_notifications_complete(context_.handle(),
                [this] {
                    const std::lock_guard lock(mutex_);
                    stop_control_scheduled_ = true;
                    context_.defer_or_terminate([this] { apply_stop(); });
                });
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] bool post_control(move_only_function<void()> control) noexcept {
        const std::lock_guard lock(mutex_);
        if (lifecycle_.state() != runtime_lifecycle::state_type::running || work_released_) {
            return false;
        }
        context_.defer_or_terminate(std::move(control));
        return true;
    }

    void finalize() noexcept {
        {
            const std::lock_guard lock(mutex_);
            if (finalization_requested_) {
                return;
            }
            finalization_requested_ = true;
        }
        request_stop();
        const std::lock_guard lock(mutex_);
        // If request_stop is still notifying listeners, its eventual control
        // consumes this request. Never release work before that publication.
        if (stop_control_scheduled_ && !work_released_) {
            context_.defer_or_terminate([this] { apply_stop(); });
        }
    }

    void apply_stop() noexcept {
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
        {
            const std::lock_guard lock(mutex_);
            if (!finalization_requested_ || work_released_) {
                return;
            }
            work_released_ = true;
        }
        try {
            if (hooks_.finalize_) {
                hooks_.finalize_();
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
        work_.reset();
        (void)lifecycle_.request_stop();
        (void)abort_phases();
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
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    worker_runtime_context context_;
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
    bool started_{};
    bool joining_{};
    bool joined_{};
    bool stop_control_scheduled_{};
    bool finalization_requested_{};
    bool work_released_{};
    bool stop_applied_{};
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

void worker_runtime::start() {
    impl_->start();
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

void worker_runtime::rethrow_failure() const {
    if (const auto error = failure()) {
        std::rethrow_exception(error);
    }
}

runtime_lifecycle::state_type worker_runtime::state() const noexcept {
    return impl_->lifecycle_.state();
}

bool worker_runtime::started() const noexcept {
    const std::lock_guard lock(impl_->mutex_);
    return impl_->started_;
}

worker_runtime_context& worker_runtime::context() & noexcept {
    return impl_->context_;
}

}  // namespace ruvia
