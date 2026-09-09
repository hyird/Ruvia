#include "ruvia/core/BlockingPool.h"

#include <algorithm>
#include <condition_variable>
#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ruvia/core/detail/util/FailureReport.h"
#include "ruvia/core/memory/PmrObject.h"
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
    struct Node final {
        explicit Node(MoveOnlyFunction<void()>&& value)
            : task(std::move(value)) {}

        MoveOnlyFunction<void()> task;
        Node* next{nullptr};
    };

    explicit Impl(const BlockingPoolOptions& options)
        : threadCount(resolveThreadCount(options.threadCount)),
          queueCapacity(resolveQueueCapacity(options.queueCapacity, threadCount)),
          nodeResource(detail::processResource()) {}

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
            std::unique_ptr<Node, detail::PmrObjectDeleter<Node>> node(nullptr,
                detail::PmrObjectDeleter<Node>{nodeResource});
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [this] { return stopping || queued != 0; });
                if (stopping || queued == 0) {
                    // Stopping discards every task that has not already been
                    // picked up. A worker must never start queued work after
                    // the stop request becomes visible.
                    return;
                }
                node.reset(queueHead);
                queueHead = node->next;
                node->next = nullptr;
                if (queueHead == nullptr) {
                    queueTail = nullptr;
                }
                --queued;
                ++running;
            }
            try {
                node->task();
            } catch (...) {
                // The blocking wrapper catches the callable's exceptions and
                // hands them back to the waiter, so reaching this is a raw
                // submit() whose task threw with nobody to receive it.
                detail::reportUnhandledFailure("blocking pool task", std::current_exception());
            }
            // The task owns the completion it answered; destroy it here rather
            // than under the lock the next iteration takes.
            node.reset();
            {
                std::lock_guard lock(mutex);
                --running;
                ++completed;
            }
        }
    }

    void stop() noexcept {
        // Only the first stop caller owns the drain. In particular, a queued
        // task's destructor may call stop() again; letting that call recurse
        // into the drain would grow the stack once per queued task.
        Node* dropped = nullptr;
        {
            std::lock_guard lock(mutex);
            if (stopping) {
                return;
            }
            stopping = true;
            discarded += queued;
            dropped = queueHead;
            queueHead = nullptr;
            queueTail = nullptr;
            queued = 0;
        }
        condition.notify_all();

        // The detached chain owns all queued callables. Destroy nodes
        // iteratively outside the mutex so callable destructors may reenter
        // stats(), stop(), or submit() safely without allocation or recursion.
        std::unique_ptr<Node, detail::PmrObjectDeleter<Node>> node(
            dropped, detail::PmrObjectDeleter<Node>{nodeResource});
        while (node != nullptr) {
            Node* next = node->next;
            node->next = nullptr;
            node.reset(next);
        }
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
    std::pmr::memory_resource* nodeResource;
    mutable std::mutex mutex;
    std::condition_variable condition;
    Node* queueHead{nullptr};
    Node* queueTail{nullptr};
    std::size_t queued{0};
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
        .queued = impl_->queued,
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
        if (impl_->queued >= impl_->queueCapacity) {
            ++impl_->rejected;
            return BlockingSubmitStatus::kQueueFull;
        }
    }
    auto node = detail::makePmrObject<Impl::Node>(impl_->nodeResource, std::move(task));
    BlockingSubmitStatus status = BlockingSubmitStatus::kAccepted;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopping) {
            ++impl_->discarded;
            status = BlockingSubmitStatus::kPoolStopped;
        } else if (impl_->queued >= impl_->queueCapacity) {
            ++impl_->rejected;
            status = BlockingSubmitStatus::kQueueFull;
        } else {
            if (impl_->queueTail != nullptr) {
                impl_->queueTail->next = node.get();
            } else {
                impl_->queueHead = node.get();
            }
            impl_->queueTail = node.release();
            ++impl_->queued;
        }
    }
    if (status != BlockingSubmitStatus::kAccepted) {
        return status;
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
