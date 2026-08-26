#include "ruvia/core/BlockingPool.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ruvia/core/detail/util/FailureReport.h"
#include "ruvia/core/memory/ProcessResource.h"

namespace ruvia {

namespace {

[[nodiscard]] std::size_t resolveThreadCount(std::size_t requested) noexcept {
    if (requested != 0) {
        return requested;
    }
    const auto hardwareThreads = std::size_t{std::thread::hardware_concurrency()};
    const auto halfHardwareThreads = (hardwareThreads + 1) / 2;
    return std::clamp(halfHardwareThreads, std::size_t{2}, std::size_t{8});
}

[[nodiscard]] std::size_t resolveQueueCapacity(
    std::size_t requested, std::size_t threadCount) noexcept {
    if (requested != 0) {
        return requested;
    }
    return threadCount * 64;
}

}  // namespace

std::string_view describeBlockingStatus(BlockingStatus status) noexcept {
    switch (status) {
        case BlockingStatus::kCompleted:
            return "completed";
        case BlockingStatus::kQueueFull:
            return "blocking pool queue is full";
        case BlockingStatus::kPoolStopped:
            return "blocking pool is stopped";
        case BlockingStatus::kWorkerStopping:
            return "worker is stopping";
        case BlockingStatus::kCancelled:
            return "blocking operation was cancelled";
        case BlockingStatus::kTimedOut:
            return "blocking operation timed out";
    }
    return "unknown";
}

BlockingOperationRejected::BlockingOperationRejected(BlockingStatus status)
    : std::runtime_error(std::string(describeBlockingStatus(status))),
      status_(status) {
    if (status == BlockingStatus::kCompleted) {
        throw std::invalid_argument("completed blocking operation was not rejected");
    }
}

struct BlockingPool::ThreadState final {
    explicit ThreadState()
        : threads(detail::processResource()) {}

    std::pmr::vector<std::thread> threads;
};

struct BlockingPool::Impl final {
    explicit Impl(const BlockingPoolOptions& options)
        : threadCount(resolveThreadCount(options.threadCount)),
          queueCapacity(resolveQueueCapacity(options.queueCapacity, threadCount)),
          queue(detail::processResource()) {}

    void start(const std::shared_ptr<Impl>& self, std::pmr::vector<std::thread>& threads) {
        threads.reserve(threadCount);
        try {
            for (std::size_t i = 0; i < threadCount; ++i) {
                threads.emplace_back([self] { self->run(); });
            }
        } catch (...) {
            // A pool that could not start all of its threads is not a pool the
            // caller asked for. Unwind the ones that did start before the
            // exception leaves construction.
            stop();
            join(threads);
            throw;
        }
    }

    void run() noexcept {
        for (;;) {
            MoveOnlyFunction<void()> task;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [this] { return stopping || !queue.empty(); });
                if (queue.empty()) {
                    // Only a stop empties the queue without work arriving.
                    return;
                }
                task = std::move(queue.front());
                queue.pop_front();
                ++running;
            }
            try {
                task();
            } catch (...) {
                // The blocking wrapper catches the callable's exceptions and
                // hands them back to the waiter, so reaching this is a raw
                // submit() whose task threw with nobody to receive it.
                detail::reportUnhandledFailure("blocking pool task", std::current_exception());
            }
            // The task owns the completion it answered; destroy it here rather
            // than under the lock the next iteration takes.
            task = nullptr;
            {
                std::lock_guard lock(mutex);
                --running;
                ++completed;
            }
        }
    }

    void stop() noexcept {
        // Use the pool's own allocator and exchange the whole queue.  stop()
        // is noexcept: moving tasks one by one into a default-constructed
        // queue could allocate and terminate the process if that allocation
        // failed.  The equal-resource swap only exchanges deque bookkeeping;
        // task destruction still happens after the mutex is released.
        std::pmr::deque<MoveOnlyFunction<void()>> dropped(queue.get_allocator().resource());
        {
            std::lock_guard lock(mutex);
            stopping = true;
            discarded += queue.size();
            // Dropped tasks answer their waiters from their own destructors,
            // which must not run under this mutex: a waiter resumed on its
            // worker may submit again.
            queue.swap(dropped);
        }
        condition.notify_all();
        dropped.clear();
    }

    static void throwIfCurrent(const std::pmr::vector<std::thread>& threads) {
        if (std::ranges::any_of(threads, [](const std::thread& thread) {
                return thread.joinable() && thread.get_id() == std::this_thread::get_id();
            })) {
            throw std::logic_error("cannot join a blocking pool from one of its threads");
        }
    }

    static void join(std::pmr::vector<std::thread>& threads) {
        throwIfCurrent(threads);
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    static void detach(std::pmr::vector<std::thread>& threads) noexcept {
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.detach();
            }
        }
    }

    std::size_t threadCount;
    std::size_t queueCapacity;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::pmr::deque<MoveOnlyFunction<void()>> queue;
    std::size_t running{0};
    std::uint64_t completed{0};
    std::uint64_t rejected{0};
    std::uint64_t discarded{0};
    bool stopping{false};
};

BlockingPool::BlockingPool(BlockingPoolOptions options)
    : impl_(std::make_shared<Impl>(options)),
      threads_(std::make_unique<ThreadState>()) {
    impl_->start(impl_, threads_->threads);
}

BlockingPool::~BlockingPool() {
    stop();
    Impl::detach(threads_->threads);
}

std::size_t BlockingPool::threadCount() const noexcept {
    return threads_->threads.size();
}

std::size_t BlockingPool::queueCapacity() const noexcept {
    return impl_->queueCapacity;
}

BlockingPoolStats BlockingPool::stats() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return BlockingPoolStats{
        .queued = impl_->queue.size(),
        .running = impl_->running,
        .completed = impl_->completed,
        .rejected = impl_->rejected,
        .discarded = impl_->discarded,
    };
}

BlockingSubmitStatus BlockingPool::submit(MoveOnlyFunction<void()> task) {
    if (!task) {
        throw std::invalid_argument("blocking pool requires a callable task");
    }
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopping) {
            ++impl_->discarded;
            return BlockingSubmitStatus::kPoolStopped;
        }
        if (impl_->queue.size() >= impl_->queueCapacity) {
            ++impl_->rejected;
            return BlockingSubmitStatus::kQueueFull;
        }
        impl_->queue.push_back(std::move(task));
    }
    impl_->condition.notify_one();
    return BlockingSubmitStatus::kAccepted;
}

void BlockingPool::stop() noexcept {
    impl_->stop();
}

void BlockingPool::join() {
    Impl::throwIfCurrent(threads_->threads);
    stop();
    Impl::join(threads_->threads);
}

}  // namespace ruvia
