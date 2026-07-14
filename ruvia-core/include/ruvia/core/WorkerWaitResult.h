#pragma once

#include <type_traits>
#include <utility>
#include <variant>

namespace ruvia::detail {
struct WorkerWaitResultAccess;

template <typename T>
class WorkerWaitValue final {
public:
    explicit WorkerWaitValue(T value)
        noexcept(std::is_nothrow_move_constructible_v<T>)
        : value_(std::move(value)) {}

    T value_;
};
}

namespace ruvia {

class WorkerWaitClosed final {
private:
    friend struct detail::WorkerWaitResultAccess;
    constexpr WorkerWaitClosed() noexcept = default;
};

class WorkerWaitStopping final {
private:
    friend struct detail::WorkerWaitResultAccess;
    constexpr WorkerWaitStopping() noexcept = default;
};

class WorkerWaitTimedOut final {
private:
    friend struct detail::WorkerWaitResultAccess;
    constexpr WorkerWaitTimedOut() noexcept = default;
};

// Worker-bound waits have four mutually exclusive outcomes. Only the value
// alternative owns a payload; close, worker shutdown, and timeout cannot carry
// a stale or fabricated value.
template <typename T>
class WorkerWaitResult final {
public:
    [[nodiscard]] const T* value() const noexcept {
        const auto* result =
            std::get_if<detail::WorkerWaitValue<T>>(&result_);
        return result == nullptr ? nullptr : &result->value_;
    }

    [[nodiscard]] T* value() noexcept {
        auto* result = std::get_if<detail::WorkerWaitValue<T>>(&result_);
        return result == nullptr ? nullptr : &result->value_;
    }

    [[nodiscard]] const WorkerWaitClosed* closed() const noexcept {
        return std::get_if<WorkerWaitClosed>(&result_);
    }

    [[nodiscard]] const WorkerWaitStopping* workerStopping() const noexcept {
        return std::get_if<WorkerWaitStopping>(&result_);
    }

    [[nodiscard]] const WorkerWaitTimedOut* timedOut() const noexcept {
        return std::get_if<WorkerWaitTimedOut>(&result_);
    }

private:
    friend struct detail::WorkerWaitResultAccess;

    using Result = std::variant<
        detail::WorkerWaitValue<T>,
        WorkerWaitClosed,
        WorkerWaitStopping,
        WorkerWaitTimedOut>;

    template <typename Alternative>
    explicit WorkerWaitResult(Alternative alternative)
        noexcept(std::is_nothrow_constructible_v<Result, Alternative&&>)
        : result_(std::move(alternative)) {}

    Result result_;
};

}  // namespace ruvia

namespace ruvia::detail {

struct WorkerWaitResultAccess final {
    template <typename T>
    [[nodiscard]] static WorkerWaitResult<T> value(T value)
        noexcept(std::is_nothrow_move_constructible_v<T>) {
        return WorkerWaitResult<T>(WorkerWaitValue<T>(std::move(value)));
    }

    template <typename T>
    [[nodiscard]] static WorkerWaitResult<T> closed() noexcept {
        return WorkerWaitResult<T>(WorkerWaitClosed());
    }

    template <typename T>
    [[nodiscard]] static WorkerWaitResult<T> workerStopping() noexcept {
        return WorkerWaitResult<T>(WorkerWaitStopping());
    }

    template <typename T>
    [[nodiscard]] static WorkerWaitResult<T> timedOut() noexcept {
        return WorkerWaitResult<T>(WorkerWaitTimedOut());
    }
};

}  // namespace ruvia::detail
