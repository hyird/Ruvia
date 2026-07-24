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
    return std::max(std::size_t{1}, std::size_t{std::thread::hardware_concurrency()});
}

[[nodiscard]] std::size_t resolveQueueCapacity(std::size_t requested, std::size_t threadCount) noexcept {
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
        case BlockingStatus::kTimedOut:
            return "blocking operation timed out";
    }
    return "unknown";
}

BlockingOperationRejected::BlockingOperationRejected(BlockingStatus status)
    : std::runtime_error(std::string(describeBlockingStatus(status))),
      status_(status) {}

struct BlockingPool::Impl final {
    explicit Impl(const BlockingPoolOptions& options)
        : queueCapacity(resolveQueueCapacity(options.queueCapacity, resolveThreadCount(options.threadCount))),
          queue(detail::processResource()),
          threads(detail::processResource()) {
        const auto count = resolveThreadCount(options.threadCount);
        threads.reserve(count);
        try {
            for (std::size_t i = 0; i < count; ++i) {
                threads.emplace_back([this] { run(); });
            }
        } catch (...) {
            // A pool that could not start all of its threads is not a pool the
            // caller asked for. Unwind the ones that did start before the
            // exception leaves the constructor, so no thread outlives *this.
            stop();
            join();
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
                // runBlocking()'s wrapper catches the callable's exceptions and
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
        std::deque<MoveOnlyFunction<void()>> dropped;
        {
            std::lock_guard lock(mutex);
            stopping = true;
            discarded += queue.size();
            // Dropped tasks answer their waiters from their own destructors,
            // which must not run under this mutex: a waiter resumed on its
            // worker may submit again.
            for (auto& task : queue) {
                dropped.push_back(std::move(task));
            }
            queue.clear();
        }
        condition.notify_all();
        dropped.clear();
    }

    void join() noexcept {
        for (auto& thread : threads) {
            if (thread.joinable()) {
                if (thread.get_id() == std::this_thread::get_id()) {
                    // Joining yourself deadlocks. A pool thread reaching here
                    // means a task called join() on its own pool.
                    std::terminate();
                }
                thread.join();
            }
        }
    }

    std::size_t queueCapacity;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::pmr::deque<MoveOnlyFunction<void()>> queue;
    std::pmr::vector<std::thread> threads;
    std::size_t running{0};
    std::uint64_t completed{0};
    std::uint64_t rejected{0};
    std::uint64_t discarded{0};
    bool stopping{false};
};

BlockingPool::BlockingPool(BlockingPoolOptions options)
    : impl_(std::make_unique<Impl>(options)) {}

BlockingPool::~BlockingPool() {
    stop();
    join();
}

std::size_t BlockingPool::threadCount() const noexcept {
    return impl_->threads.size();
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
            return BlockingSubmitStatus::kStopped;
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

void BlockingPool::join() noexcept {
    impl_->join();
}

}  // namespace ruvia
