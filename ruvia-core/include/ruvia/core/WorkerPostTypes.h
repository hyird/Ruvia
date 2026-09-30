#pragma once

#include <cstdint>
#include <stdexcept>
#include <utility>

#include "ruvia/core/MoveOnlyFunction.h"

namespace ruvia {

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

}  // namespace ruvia
