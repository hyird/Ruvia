#pragma once

#include <cstdint>
#include <exception>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace ruvia::detail {
struct WorkerWaitResultAccess;

template <typename T>
class WorkerWaitValue final {
public:
    explicit WorkerWaitValue(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
        : value_(std::move(value)) {}

    T value_;
};
}  // namespace ruvia::detail

namespace ruvia {

enum class WorkerWaitStatus : std::uint8_t {
    kValue,
    kClosed,
    kWorkerStopping,
    kTimedOut,
    kCancelled,
};

// Worker-bound waits have five mutually exclusive outcomes. A status is always
// available for exhaustive handling; only kValue owns a payload.
template <typename T>
class WorkerWaitResult final {
public:
    [[nodiscard]] WorkerWaitStatus status() const noexcept {
        return std::holds_alternative<detail::WorkerWaitValue<T>>(result_)
            ? WorkerWaitStatus::kValue
            : std::get<WorkerWaitStatus>(result_);
    }

    [[nodiscard]] bool hasValue() const noexcept {
        return std::holds_alternative<detail::WorkerWaitValue<T>>(result_);
    }

    [[nodiscard]] const T& value() const& {
        const auto* result = std::get_if<detail::WorkerWaitValue<T>>(&result_);
        if (result == nullptr) {
            throw std::logic_error("worker wait result has no value");
        }
        return result->value_;
    }
    const T& value() const&& = delete;

    [[nodiscard]] T& value() & {
        auto* result = std::get_if<detail::WorkerWaitValue<T>>(&result_);
        if (result == nullptr) {
            throw std::logic_error("worker wait result has no value");
        }
        return result->value_;
    }
    T& value() && = delete;

    [[nodiscard]] T takeValue() && {
        auto* result = std::get_if<detail::WorkerWaitValue<T>>(&result_);
        if (result == nullptr) {
            throw std::logic_error("worker wait result has no value");
        }
        return std::move(result->value_);
    }

private:
    friend struct detail::WorkerWaitResultAccess;

    using Result = std::variant<detail::WorkerWaitValue<T>, WorkerWaitStatus>;

    explicit WorkerWaitResult(detail::WorkerWaitValue<T> value) noexcept(std::is_nothrow_move_constructible_v<T>)
        : result_(std::move(value)) {}

    explicit WorkerWaitResult(WorkerWaitStatus status) noexcept
        : result_(status) {
        if (status == WorkerWaitStatus::kValue) {
            std::terminate();
        }
    }

    Result result_;
};

}  // namespace ruvia

namespace ruvia::detail {

struct WorkerWaitResultAccess final {
    template <typename T>
    [[nodiscard]] static WorkerWaitResult<T> value(T value) noexcept(std::is_nothrow_move_constructible_v<T>) {
        return WorkerWaitResult<T>(WorkerWaitValue<T>(std::move(value)));
    }

    template <typename T>
    [[nodiscard]] static WorkerWaitResult<T> outcome(WorkerWaitStatus status) noexcept {
        return WorkerWaitResult<T>(status);
    }
};

}  // namespace ruvia::detail
