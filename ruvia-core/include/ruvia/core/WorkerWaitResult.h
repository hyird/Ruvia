#pragma once

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

class WorkerWaitCancelled final {
private:
    friend struct detail::WorkerWaitResultAccess;
    constexpr WorkerWaitCancelled() noexcept = default;
};

// Worker-bound waits have five mutually exclusive outcomes. Only the value
// alternative owns a payload; close, worker shutdown, and timeout cannot carry
// a stale or fabricated value, nor can caller cancellation.
template <typename T>
class WorkerWaitResult final {
public:
    [[nodiscard]] const T* value() const& noexcept {
        const auto* result = std::get_if<detail::WorkerWaitValue<T>>(&result_);
        return result == nullptr ? nullptr : &result->value_;
    }
    [[nodiscard]] const T* value() const&& = delete;

    [[nodiscard]] T* value() & noexcept {
        auto* result = std::get_if<detail::WorkerWaitValue<T>>(&result_);
        return result == nullptr ? nullptr : &result->value_;
    }
    [[nodiscard]] T* value() && = delete;

    [[nodiscard]] const WorkerWaitClosed* closed() const& noexcept {
        return std::get_if<WorkerWaitClosed>(&result_);
    }
    [[nodiscard]] const WorkerWaitClosed* closed() const&& = delete;

    [[nodiscard]] const WorkerWaitStopping* workerStopping() const& noexcept {
        return std::get_if<WorkerWaitStopping>(&result_);
    }
    [[nodiscard]] const WorkerWaitStopping* workerStopping() const&& = delete;

    [[nodiscard]] const WorkerWaitTimedOut* timedOut() const& noexcept {
        return std::get_if<WorkerWaitTimedOut>(&result_);
    }
    [[nodiscard]] const WorkerWaitTimedOut* timedOut() const&& = delete;

    [[nodiscard]] const WorkerWaitCancelled* cancelled() const& noexcept {
        return std::get_if<WorkerWaitCancelled>(&result_);
    }
    [[nodiscard]] const WorkerWaitCancelled* cancelled() const&& = delete;

    // Consumes the value alternative, matching the takeRejected() pattern of
    // the channel/one-shot results. Only the value outcome carries a payload;
    // any other outcome makes this a programming error.
    [[nodiscard]] T takeValue() && noexcept(std::is_nothrow_move_constructible_v<T>) {
        auto* result = std::get_if<detail::WorkerWaitValue<T>>(&result_);
        if (result == nullptr) {
            std::terminate();
        }
        return std::move(result->value_);
    }

private:
    friend struct detail::WorkerWaitResultAccess;

    using Result = std::variant<detail::WorkerWaitValue<T>, WorkerWaitClosed, WorkerWaitStopping, WorkerWaitTimedOut, WorkerWaitCancelled>;

    template <typename Alternative>
    explicit WorkerWaitResult(Alternative alternative) noexcept(std::is_nothrow_constructible_v<Result, Alternative&&>)
        : result_(std::move(alternative)) {}

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

    template <typename T>
    [[nodiscard]] static WorkerWaitResult<T> cancelled() noexcept {
        return WorkerWaitResult<T>(WorkerWaitCancelled());
    }
};

}  // namespace ruvia::detail
