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

template <typename t_type>
class task;
class task_scope;

namespace detail {

[[nodiscard]] void* task_frame_allocate(std::size_t bytes_value);
void task_frame_deallocate(void* pointer) noexcept;
void task_frame_deallocate_sized(void* pointer, std::size_t bytes_value) noexcept;

template <typename t_type>
class task_promise;
template <typename t_type>
class task_awaiter;
template <typename t_type, typename handler_type>
class task_completion_state;

template <typename t_type>
concept asio_task_result = std::is_void_v<t_type> || std::is_nothrow_move_constructible_v<t_type>;

template <typename t_type, typename completion_token_type>
    requires asio_task_result<t_type>
auto async_start_task(task<t_type>&& task_value, completion_token_type&& token);

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

    void set_completion(void* state_value, void (*completion)(void*) noexcept) noexcept {
        completion_state_ = state_value;
        completion_ = completion;
    }

    [[nodiscard]] std::coroutine_handle<> finish() const noexcept {
        // Completion can destroy the containing coroutine frame. Snapshot every
        // input and never access this control again after invoking the callback.
        const auto completion = completion_;
        if (completion != nullptr) {
            const auto state_value = completion_state_;
            completion(state_value);
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

struct task_final_awaiter final {
    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }

    template <typename promise_type>
    [[nodiscard]] std::coroutine_handle<> await_suspend(
        std::coroutine_handle<promise_type> handle) const noexcept {
        return handle.promise().control_.finish();
    }

    void await_resume() const noexcept {}
};

struct task_promise_pending final {};
struct task_promise_completed final {};

template <typename t_type>
class task_promise_value final {
public:
    template <typename u_type>
        requires std::constructible_from<t_type, u_type>
    explicit task_promise_value(u_type&& value) noexcept(std::is_nothrow_constructible_v<t_type, u_type>)
        : value_(std::forward<u_type>(value)) {}

    [[nodiscard]] t_type take_value() && {
        return std::move(value_);
    }

private:
    t_type value_;
};

class task_promise_failure final {
public:
    explicit task_promise_failure(std::exception_ptr exception) noexcept
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
        state_.template emplace<task_promise_failure>(std::current_exception());
    }

    result_type result() {
        if (const auto* failure = std::get_if<task_promise_failure>(&state_)) [[unlikely]] {
            std::rethrow_exception(failure->exception());
        }
        if constexpr (std::is_void_v<result_type>) {
            assert(std::holds_alternative<completed_type>(state_));
        } else {
            auto* value = std::get_if<completed_type>(&state_);
            assert(value != nullptr);
            return std::move(*value).take_value();
        }
    }

private:
    using completed_type = std::conditional_t<std::is_void_v<result_type>,
        task_promise_completed, task_promise_value<result_type>>;
    std::variant<task_promise_pending, completed_type, task_promise_failure> state_;
};

// Coroutine protocol requires separate return_value / return_void declarations.
// These are signature adapters, not separate ownership or completion engines.
template <typename t_type>
class task_promise final {
public:
    using value_type = t_type;
    static_assert(!std::is_reference_v<t_type>, "ruvia::task does not support reference result types");
    task_promise() noexcept = default;

    static void* operator new(std::size_t size) {
        return task_frame_allocate(size);
    }
    static void operator delete(void* pointer) noexcept {
        task_frame_deallocate(pointer);
    }
    static void operator delete(void* pointer, std::size_t size) noexcept {
        task_frame_deallocate_sized(pointer, size);
    }

    [[nodiscard]] task<t_type> get_return_object() noexcept;
    [[nodiscard]] std::suspend_always initial_suspend() const noexcept {
        return {};
    }
    [[nodiscard]] task_final_awaiter final_suspend() noexcept {
        return {};
    }

    template <typename u_type>
        requires std::constructible_from<t_type, u_type>
    void return_value(u_type&& value) noexcept(std::is_nothrow_constructible_v<t_type, u_type>) {
        state_.complete(std::forward<u_type>(value));
    }
    void unhandled_exception() noexcept {
        state_.fail();
    }
    [[nodiscard]] t_type result() & {
        return state_.result();
    }

private:
    template <typename>
    friend class ruvia::task;
    template <typename>
    friend class task_awaiter;
    template <typename, typename>
    friend class task_completion_state;
    friend class ruvia::task_scope;
    friend struct task_final_awaiter;

    task_promise_result<t_type> state_;
    task_frame_control control_;
};

template <>
class task_promise<void> final {
public:
    task_promise() noexcept = default;
    static void* operator new(std::size_t size) {
        return task_frame_allocate(size);
    }
    static void operator delete(void* pointer) noexcept {
        task_frame_deallocate(pointer);
    }
    static void operator delete(void* pointer, std::size_t size) noexcept {
        task_frame_deallocate_sized(pointer, size);
    }

    [[nodiscard]] task<void> get_return_object() noexcept;
    [[nodiscard]] std::suspend_always initial_suspend() const noexcept {
        return {};
    }
    [[nodiscard]] task_final_awaiter final_suspend() noexcept {
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
    friend class ruvia::task;
    template <typename>
    friend class task_awaiter;
    template <typename, typename>
    friend class task_completion_state;
    friend class ruvia::task_scope;
    friend struct task_final_awaiter;

    task_promise_result<void> state_;
    task_frame_control control_;
};

}  // namespace detail
}  // namespace ruvia
