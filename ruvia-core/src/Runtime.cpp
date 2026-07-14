#include <ruvia/core/Runtime.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>

#include <ruvia/core/detail/WorkerDispatcher.h>
#include <ruvia/core/detail/WorkerSelection.h>

namespace ruvia {
namespace {

enum class RuntimeState : std::uint8_t { kReady, kRunning, kStopping, kStopped };

std::size_t defaultWorkerCount() noexcept {
    return std::max<std::size_t>(1, std::thread::hardware_concurrency());
}

}

struct Runtime::Impl {
    struct Worker {
        explicit Worker(std::size_t mailboxCapacity)
            : work(asio::make_work_guard(ioContext)),
              dispatcher(std::make_shared<detail::WorkerDispatcher>(ioContext,
                                                                    mailboxCapacity)),
              handle(detail::WorkerHandleAccess::make(dispatcher)) {}

        asio::io_context ioContext;
        asio::executor_work_guard<asio::io_context::executor_type> work;
        std::shared_ptr<detail::WorkerDispatcher> dispatcher;
        WorkerHandle handle;
        std::thread thread;
    };

    explicit Impl(RuntimeOptions options) {
        const auto count = options.workerCount == 0 ? defaultWorkerCount() : options.workerCount;
        if (options.mailboxCapacity == 0) {
            throw std::invalid_argument("runtime mailbox capacity must be greater than zero");
        }
        workers.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            workers.push_back(std::make_unique<Worker>(options.mailboxCapacity));
        }
    }

    void recordFailure(std::exception_ptr failure) noexcept {
        std::lock_guard lock(failureMutex);
        if (!firstFailure) {
            firstFailure = std::move(failure);
        }
    }

    void stop() noexcept {
        const auto previous = state.exchange(RuntimeState::kStopping, std::memory_order_acq_rel);
        if (previous == RuntimeState::kStopping || previous == RuntimeState::kStopped) {
            return;
        }
        for (const auto& worker : workers) {
            worker->dispatcher->close();
            try {
                if (worker->dispatcher->isCurrent()) {
                    worker->dispatcher->stopTimers();
                } else {
                    worker->dispatcher->defer(
                        [dispatcher = worker->dispatcher] { dispatcher->stopTimers(); });
                }
            } catch (...) {
            }
            worker->work.reset();
        }
    }

    std::vector<std::unique_ptr<Worker>> workers;
    std::atomic<RuntimeState> state{RuntimeState::kReady};
    std::atomic<std::size_t> nextIndex{0};
    std::mutex failureMutex;
    std::exception_ptr firstFailure;
};

Runtime::Runtime(RuntimeOptions options) : impl_(std::make_unique<Impl>(options)) {}

Runtime::~Runtime() {
    stop();
    try {
        join();
    } catch (...) {
    }
}

void Runtime::start() {
    auto expected = RuntimeState::kReady;
    if (!impl_->state.compare_exchange_strong(expected,
                                              RuntimeState::kRunning,
                                              std::memory_order_acq_rel)) {
        throw std::logic_error("runtime can only be started once");
    }
    try {
        for (const auto& worker : impl_->workers) {
            worker->thread = std::thread([this, worker = worker.get()] {
                try {
                    worker->ioContext.run();
                } catch (...) {
                    impl_->recordFailure(std::current_exception());
                    impl_->stop();
                }
            });
        }
    } catch (...) {
        impl_->stop();
        for (const auto& worker : impl_->workers) {
            if (worker->thread.joinable()) {
                worker->thread.join();
            }
        }
        impl_->state.store(RuntimeState::kStopped, std::memory_order_release);
        throw;
    }
}

void Runtime::stop() noexcept {
    impl_->stop();
}

void Runtime::join() {
    if (impl_->state.load(std::memory_order_acquire) == RuntimeState::kReady) {
        impl_->stop();
    }
    for (const auto& worker : impl_->workers) {
        if (worker->thread.joinable()) {
            worker->thread.join();
        }
    }
    impl_->state.store(RuntimeState::kStopped, std::memory_order_release);

    std::exception_ptr failure;
    {
        std::lock_guard lock(impl_->failureMutex);
        failure = std::exchange(impl_->firstFailure, nullptr);
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

std::size_t Runtime::workerCount() const noexcept { return impl_->workers.size(); }

WorkerHandle Runtime::worker(std::size_t index) const {
    if (index >= impl_->workers.size()) {
        throw std::out_of_range("runtime worker index is out of range");
    }
    return impl_->workers[index]->handle;
}

WorkerHandle Runtime::nextWorker() noexcept {
    const auto index = impl_->nextIndex.fetch_add(1, std::memory_order_relaxed);
    return impl_->workers[index % impl_->workers.size()]->handle;
}

WorkerHandle Runtime::workerFor(std::uint64_t key) const noexcept {
    return impl_->workers[key % impl_->workers.size()]->handle;
}

WorkerHandle Runtime::workerFor(std::string_view key) const noexcept {
    return workerFor(detail::workerSelectionHash(key));
}

}
