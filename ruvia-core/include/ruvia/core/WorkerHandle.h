#pragma once

#include <concepts>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

#include <ruvia/core/MoveOnlyFunction.h>

namespace ruvia {

using WorkerId = std::uint64_t;

enum class PostStatus : std::uint8_t {
    kAccepted,
    kQueueFull,
    kWorkerStopping,
};

template <typename Signature>
class PostOutcome final {
public:
    using Task = MoveOnlyFunction<Signature>;

    PostOutcome(const PostOutcome&) = delete;
    PostOutcome& operator=(const PostOutcome&) = delete;
    PostOutcome(PostOutcome&&) noexcept = default;
    PostOutcome& operator=(PostOutcome&&) noexcept = default;

    [[nodiscard]] PostStatus status() const noexcept {
        return status_;
    }

    [[nodiscard]] bool accepted() const noexcept {
        return status_ == PostStatus::kAccepted;
    }

    [[nodiscard]] Task* rejected() & noexcept {
        return rejected_ ? &rejected_ : nullptr;
    }

    [[nodiscard]] const Task* rejected() const& noexcept {
        return rejected_ ? &rejected_ : nullptr;
    }

    Task* rejected() && = delete;
    const Task* rejected() const&& = delete;

    [[nodiscard]] Task takeRejected() && {
        if (status_ == PostStatus::kAccepted || !rejected_) {
            throw std::logic_error("accepted post outcome has no rejected task");
        }
        return std::move(rejected_);
    }

    [[nodiscard]] static PostOutcome accept() noexcept {
        return PostOutcome(PostStatus::kAccepted);
    }

    [[nodiscard]] static PostOutcome reject(PostStatus status, Task task) {
        if (status == PostStatus::kAccepted) {
            throw std::invalid_argument("rejected post outcome requires a rejection status");
        }
        if (!task) {
            throw std::invalid_argument("rejected post outcome requires a callable task");
        }
        return PostOutcome(status, std::move(task));
    }

    friend bool operator==(const PostOutcome& outcome, PostStatus status) noexcept {
        return outcome.status_ == status;
    }

    friend bool operator==(PostStatus status, const PostOutcome& outcome) noexcept {
        return outcome == status;
    }

private:
    explicit PostOutcome(PostStatus status) noexcept
        : status_(status) {}

    PostOutcome(PostStatus status, Task task) noexcept
        : status_(status),
          rejected_(std::move(task)) {}

    PostStatus status_;
    Task rejected_;
};

using PostResult = PostOutcome<void()>;

namespace detail {
class WorkerDispatcher;
class WorkerShutdownListener;
class WorkerTimerRegistration;
enum class WorkerTimerOutcome : std::uint8_t;
struct WorkerHandleAccess;
}  // namespace detail

class WorkerHandle {
public:
    WorkerHandle() noexcept = default;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] bool isCurrent() const noexcept;
    [[nodiscard]] WorkerId id() const noexcept;

    template <typename Fn>
        requires detail::MoveOnlyFunctionTarget<void, Fn>
    [[nodiscard]] PostResult post(Fn&& fn) const {
        return postTask(MoveOnlyFunction<void()>(std::forward<Fn>(fn)));
    }

private:
    explicit WorkerHandle(std::shared_ptr<detail::WorkerDispatcher> dispatcher) noexcept;
    [[nodiscard]] PostResult postTask(MoveOnlyFunction<void()> task) const;

    // A handle owns the stable dispatcher endpoint, not the worker's io_context.
    // The worker detaches that endpoint before destroying its execution context.
    std::shared_ptr<detail::WorkerDispatcher> dispatcher_;
    friend struct detail::WorkerHandleAccess;
};

namespace detail {

struct WorkerHandleAccess {
    [[nodiscard]] static WorkerHandle make(const std::shared_ptr<WorkerDispatcher>& dispatcher) noexcept;
    static void defer(const WorkerHandle& worker, MoveOnlyFunction<void()> task);
    [[nodiscard]] static bool deferIfAttached(const WorkerHandle& worker, MoveOnlyFunction<void()> task);
    static void deferOrTerminate(const WorkerHandle& worker, MoveOnlyFunction<void()> task) noexcept;
    static void registerShutdownListener(const WorkerHandle& worker, const std::shared_ptr<WorkerShutdownListener>& listener);
    static void scheduleTimer(const WorkerHandle& worker, WorkerTimerRegistration& registration, std::chrono::steady_clock::time_point deadline, MoveOnlyFunction<void(WorkerTimerOutcome)> completion);
    [[nodiscard]] static PostStatus postFactory(const WorkerHandle& worker, MoveOnlyFunction<MoveOnlyFunction<void()>()> factory);
};

}  // namespace detail

}  // namespace ruvia
