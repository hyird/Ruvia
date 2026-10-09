#pragma once

#include <cstdint>
#include <stdexcept>
#include <utility>

#include "ruvia/core/move_only_function.h"

namespace ruvia {

enum class post_status : std::uint8_t {
    accepted,
    queue_full,
    worker_stopping,
};

template <typename signature_type>
class post_outcome final {
public:
    using task = move_only_function<signature_type>;

    post_outcome(const post_outcome&) = delete;
    post_outcome& operator=(const post_outcome&) = delete;
    post_outcome(post_outcome&&) noexcept = default;
    post_outcome& operator=(post_outcome&&) noexcept = default;

    [[nodiscard]] post_status status() const noexcept {
        return status_;
    }
    [[nodiscard]] bool accepted() const noexcept {
        return status_ == post_status::accepted;
    }
    [[nodiscard]] task* rejected() & noexcept {
        return rejected_ ? &rejected_ : nullptr;
    }
    [[nodiscard]] const task* rejected() const& noexcept {
        return rejected_ ? &rejected_ : nullptr;
    }
    task* rejected() && = delete;
    const task* rejected() const&& = delete;

    [[nodiscard]] task take_rejected() && {
        if (status_ == post_status::accepted || !rejected_) {
            throw std::logic_error("accepted post outcome has no rejected task");
        }
        return std::move(rejected_);
    }

    [[nodiscard]] static post_outcome accept() noexcept {
        return post_outcome(post_status::accepted);
    }
    [[nodiscard]] static post_outcome reject(post_status status, task task_value) {
        if (status == post_status::accepted) {
            throw std::invalid_argument("rejected post outcome requires a rejection status");
        }
        if (!task_value) {
            throw std::invalid_argument("rejected post outcome requires a callable task");
        }
        return post_outcome(status, std::move(task_value));
    }
    friend bool operator==(const post_outcome& outcome, post_status status) noexcept {
        return outcome.status_ == status;
    }
    friend bool operator==(post_status status, const post_outcome& outcome) noexcept {
        return outcome == status;
    }

private:
    explicit post_outcome(post_status status) noexcept
        : status_(status) {}
    post_outcome(post_status status, task task_value) noexcept
        : status_(status),
          rejected_(std::move(task_value)) {}
    post_status status_;
    task rejected_;
};

using post_result_type = post_outcome<void()>;

}  // namespace ruvia
