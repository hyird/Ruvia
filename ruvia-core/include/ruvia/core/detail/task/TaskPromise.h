#pragma once

#include <cassert>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <type_traits>
#include <utility>
#include <variant>

namespace ruvia {

template <typename T>
class Task;
class TaskScope;

namespace detail {

[[nodiscard]] void* taskFrameAllocate(std::size_t bytes);
void taskFrameDeallocate(void* pointer) noexcept;
void taskFrameDeallocateSized(void* pointer, std::size_t bytes) noexcept;

template <typename T>
class TaskPromise;
template <typename T>
class TaskAwaiter;
template <typename T, typename Handler>
class TaskCompletionState;

template <typename T>
concept AsioTaskResult = std::is_void_v<T> || std::is_nothrow_move_constructible_v<T>;

template <typename T, typename CompletionToken>
    requires AsioTaskResult<T>
auto asyncStartTask(Task<T>&& task, CompletionToken&& token);

// The frame is linear: cold frames may be discarded, started frames must finish.
// Both promise signatures compose this exact control state; it never moves.
class task_frame_control final {
public:
    task_frame_control() noexcept = default;
    task_frame_control(const task_frame_control&) = delete;
    task_frame_control& operator=(const task_frame_control&) = delete;
    task_frame_control(task_frame_control&&) = delete;
    task_frame_control& operator=(task_frame_control&&) = delete;

    void set_continuation(std::coroutine_handle<> continuation) noexcept {
        continuation_ = continuation;
    }

    void mark_started() noexcept {
        assert(phase_ == phase::cold);
        phase_ = phase::started;
    }

    [[nodiscard]] bool started() const noexcept {
        return phase_ == phase::started;
    }

    void set_completion(void* state, void (*completion)(void*) noexcept) noexcept {
        completion_state_ = state;
        completion_ = completion;
    }

    [[nodiscard]] std::coroutine_handle<> finish() const noexcept {
        // Completion can destroy the containing coroutine frame. Snapshot every
        // input and never access this control again after invoking the callback.
        const auto completion = completion_;
        if (completion != nullptr) {
            const auto state = completion_state_;
            completion(state);
            return std::noop_coroutine();
        }
        return continuation_;
    }

private:
    enum class phase : std::uint8_t { cold,
        started };
    phase phase_{phase::cold};
    std::coroutine_handle<> continuation_{std::noop_coroutine()};
    void* completion_state_{nullptr};
    void (*completion_)(void*) noexcept {nullptr};
};

struct TaskFinalAwaiter final {
    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }

    template <typename Promise>
    [[nodiscard]] std::coroutine_handle<> await_suspend(
        std::coroutine_handle<Promise> handle) const noexcept {
        return handle.promise().control_.finish();
    }

    void await_resume() const noexcept {}
};

struct TaskPromisePending final {};
struct TaskPromiseCompleted final {};

template <typename T>
class TaskPromiseValue final {
public:
    template <typename U>
        requires std::constructible_from<T, U>
    explicit TaskPromiseValue(U&& value) noexcept(std::is_nothrow_constructible_v<T, U>)
        : value_(std::forward<U>(value)) {}

    [[nodiscard]] T takeValue() && {
        return std::move(value_);
    }

private:
    T value_;
};

class TaskPromiseFailure final {
public:
    explicit TaskPromiseFailure(std::exception_ptr exception) noexcept
        : exception_(std::move(exception)) {}

    [[nodiscard]] const std::exception_ptr& exception() const& noexcept {
        return exception_;
    }
    const std::exception_ptr& exception() const&& = delete;

private:
    std::exception_ptr exception_;
};

// Void changes only the successful alternative. Failure storage, publication
// and observation have one implementation, with no additional result moves.
template <typename result_type>
class task_promise_result final {
public:
    void complete() noexcept
        requires std::is_void_v<result_type>
    {
        state_.template emplace<completed_type>();
    }

    template <typename input_type>
        requires(!std::is_void_v<result_type> && std::constructible_from<result_type, input_type>)
    void complete(input_type&& value) noexcept(std::is_nothrow_constructible_v<result_type, input_type>) {
        state_.template emplace<completed_type>(std::forward<input_type>(value));
    }

    void fail() noexcept {
        state_.template emplace<TaskPromiseFailure>(std::current_exception());
    }

    result_type result() {
        if (const auto* failure = std::get_if<TaskPromiseFailure>(&state_)) [[unlikely]] {
            std::rethrow_exception(failure->exception());
        }
        if constexpr (std::is_void_v<result_type>) {
            assert(std::holds_alternative<completed_type>(state_));
        } else {
            auto* value = std::get_if<completed_type>(&state_);
            assert(value != nullptr);
            return std::move(*value).takeValue();
        }
    }

private:
    using completed_type = std::conditional_t<std::is_void_v<result_type>,
        TaskPromiseCompleted, TaskPromiseValue<result_type>>;
    std::variant<TaskPromisePending, completed_type, TaskPromiseFailure> state_;
};

// Coroutine protocol requires separate return_value / return_void declarations.
// These are signature adapters, not separate ownership or completion engines.
template <typename T>
class TaskPromise final {
public:
    using value_type = T;
    static_assert(!std::is_reference_v<T>, "ruvia::Task<T> does not support reference result types");
    TaskPromise() noexcept = default;

    static void* operator new(std::size_t size) {
        return taskFrameAllocate(size);
    }
    static void operator delete(void* pointer) noexcept {
        taskFrameDeallocate(pointer);
    }
    static void operator delete(void* pointer, std::size_t size) noexcept {
        taskFrameDeallocateSized(pointer, size);
    }

    [[nodiscard]] Task<T> get_return_object() noexcept;
    [[nodiscard]] std::suspend_always initial_suspend() const noexcept {
        return {};
    }
    [[nodiscard]] TaskFinalAwaiter final_suspend() noexcept {
        return {};
    }

    template <typename U>
        requires std::constructible_from<T, U>
    void return_value(U&& value) noexcept(std::is_nothrow_constructible_v<T, U>) {
        state_.complete(std::forward<U>(value));
    }
    void unhandled_exception() noexcept {
        state_.fail();
    }
    [[nodiscard]] T result() & {
        return state_.result();
    }

private:
    template <typename>
    friend class ruvia::Task;
    template <typename>
    friend class TaskAwaiter;
    template <typename, typename>
    friend class TaskCompletionState;
    friend class ruvia::TaskScope;
    friend struct TaskFinalAwaiter;

    task_promise_result<T> state_;
    task_frame_control control_;
};

template <>
class TaskPromise<void> final {
public:
    TaskPromise() noexcept = default;
    static void* operator new(std::size_t size) {
        return taskFrameAllocate(size);
    }
    static void operator delete(void* pointer) noexcept {
        taskFrameDeallocate(pointer);
    }
    static void operator delete(void* pointer, std::size_t size) noexcept {
        taskFrameDeallocateSized(pointer, size);
    }

    [[nodiscard]] Task<void> get_return_object() noexcept;
    [[nodiscard]] std::suspend_always initial_suspend() const noexcept {
        return {};
    }
    [[nodiscard]] TaskFinalAwaiter final_suspend() noexcept {
        return {};
    }
    void return_void() noexcept {
        state_.complete();
    }
    void unhandled_exception() noexcept {
        state_.fail();
    }
    void result() {
        state_.result();
    }

private:
    template <typename>
    friend class ruvia::Task;
    template <typename>
    friend class TaskAwaiter;
    template <typename, typename>
    friend class TaskCompletionState;
    friend class ruvia::TaskScope;
    friend struct TaskFinalAwaiter;

    task_promise_result<void> state_;
    task_frame_control control_;
};

}  // namespace detail
}  // namespace ruvia
