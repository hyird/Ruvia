#include "ruvia/web/detail/server/UdpIngressRuntime.h"

#include <stdexcept>
#include <utility>

#include <asio/post.hpp>

namespace ruvia::detail {

UdpIngressRuntime::UdpIngressRuntime(void* failureTarget, FailureCallback failureCallback)
    : workGuard_(asio::make_work_guard(ioContext_)),
      failureTarget_(failureTarget),
      failureCallback_(failureCallback) {}

UdpIngressRuntime::~UdpIngressRuntime() {
    stop();
    std::thread thread;
    {
        std::lock_guard lock(mutex_);
        // The App lifecycle owns this runtime and destroys it only after the
        // ingress thread has been joined. Destruction on that thread is a
        // contract violation: detaching would leave run() using dead state.
        if (thread_.joinable() && thread_.get_id() == std::this_thread::get_id()) {
            std::terminate();
        }
        if (thread_.joinable()) {
            thread = std::move(thread_);
        }
    }
    if (thread.joinable()) {
        thread.join();
    }
}

void UdpIngressRuntime::launch() {
    std::lock_guard lock(mutex_);
    if (!lifecycle_.start()) {
        throw std::logic_error("UDP ingress runtime can only be launched once");
    }
    try {
        thread_ = std::thread([this] { run(); });
    } catch (...) {
        (void)lifecycle_.requestStop();
        lifecycle_.completeStop();
        condition_.notify_all();
        throw;
    }
}

void UdpIngressRuntime::waitUntilReady() {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] {
        const auto current = lifecycle_.state();
        return ready_ || current == ruvia::RuntimeLifecycle::State::kStopping ||
               current == ruvia::RuntimeLifecycle::State::kStopped;
    });
}

void UdpIngressRuntime::requestServe() {
    {
        std::lock_guard lock(mutex_);
        if (lifecycle_.state() != ruvia::RuntimeLifecycle::State::kRunning || serveRequested_) {
            return;
        }
        serveRequested_ = true;
    }
    asio::post(ioContext_, [this] {
        std::lock_guard lock(mutex_);
        serving_ = lifecycle_.state() == ruvia::RuntimeLifecycle::State::kRunning;
        condition_.notify_all();
    });
}

bool UdpIngressRuntime::waitUntilServing() {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] {
        const auto current = lifecycle_.state();
        return serving_ || failure_ != nullptr ||
               current == ruvia::RuntimeLifecycle::State::kStopping ||
               current == ruvia::RuntimeLifecycle::State::kStopped;
    });
    return serving_ && failure_ == nullptr;
}

void UdpIngressRuntime::stop() noexcept {
    bool postStop = false;
    {
        std::lock_guard lock(mutex_);
        if (!lifecycle_.requestStop()) {
            return;
        }
        if (thread_.joinable()) {
            postStop = true;
        } else {
            workGuard_.reset();
            lifecycle_.completeStop();
            condition_.notify_all();
        }
    }
    if (!postStop) {
        return;
    }
    try {
        asio::post(ioContext_, [this] {
            workGuard_.reset();
            std::lock_guard lock(mutex_);
            condition_.notify_all();
        });
    } catch (...) {
        try {
            ioContext_.stop();
        } catch (...) {
            // The ingress thread cannot be woken; never leave join() hung.
            std::terminate();
        }
    }
}

void UdpIngressRuntime::join() {
    std::thread thread;
    {
        std::lock_guard lock(mutex_);
        if (thread_.joinable() && thread_.get_id() == std::this_thread::get_id()) {
            throw std::logic_error("cannot join UDP ingress runtime from its own thread");
        }
        if (thread_.joinable()) {
            thread = std::move(thread_);
        }
    }
    if (thread.joinable()) {
        thread.join();
    }
    {
        std::lock_guard lock(mutex_);
        if (lifecycle_.state() == ruvia::RuntimeLifecycle::State::kStopping) {
            lifecycle_.completeStop();
        }
        condition_.notify_all();
    }
}

void UdpIngressRuntime::run() noexcept {
    {
        std::lock_guard lock(mutex_);
        ready_ = true;
        condition_.notify_all();
    }
    try {
        ioContext_.run();
    } catch (...) {
        {
            std::lock_guard lock(mutex_);
            if (failure_ == nullptr) {
                failure_ = std::current_exception();
            }
            (void)lifecycle_.requestStop();
            serving_ = false;
            condition_.notify_all();
        }
        if (failureCallback_ != nullptr) {
            failureCallback_(failureTarget_);
        }
    }
    workGuard_.reset();
    {
        std::lock_guard lock(mutex_);
        (void)lifecycle_.requestStop();
        lifecycle_.completeStop();
        condition_.notify_all();
    }
}

std::exception_ptr UdpIngressRuntime::failure() const noexcept {
    std::lock_guard lock(mutex_);
    return failure_;
}

void UdpIngressRuntime::rethrowFailure() const {
    if (auto exception = failure()) {
        std::rethrow_exception(exception);
    }
}

}  // namespace ruvia::detail
