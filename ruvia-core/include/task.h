#pragma once

#include <cassert>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace ruvia {

template <typename t_type>
class task;
template <typename t_type>
class task_awaiter;
class task_scope;
class operation_scope;

namespace detail {

// Internal registration composed by ruvia::task and operation_scope::drain_guard.
// Unscoped tasks never touch a scope list; scoped tasks relink in O(1) on move.
class scoped_operation_registration final {
public:
    scoped_operation_registration() noexcept = default;
    scoped_operation_registration(scoped_operation_registration&& other, void* new_frame_target) noexcept;
    ~scoped_operation_registration();

    scoped_operation_registration(const scoped_operation_registration&) = delete;
    scoped_operation_registration& operator=(const scoped_operation_registration&) = delete;
    scoped_operation_registration(scoped_operation_registration&&) = delete;
    scoped_operation_registration& operator=(scoped_operation_registration&&) = delete;

    [[nodiscard]] bool scoped() const noexcept {
        return phase_ != phase::unscoped;
    }

    void bind(operation_scope& scope, void* target, void (*retire_cold)(void*) noexcept) noexcept;
    void prepare_start() const;
    void start() noexcept;
    void prepare_completion() noexcept;
    void complete() noexcept;
    void retire_frame() noexcept;

private:
    friend class ::ruvia::operation_scope;
    enum class phase : std::uint8_t {
        unscoped,
        cold,
        running,
        completing,
        retiring,
        complete,
        expired,
    };
    void clear_frame_binding() noexcept;

    operation_scope* scope_{nullptr};
    scoped_operation_registration* previous_{nullptr};
    scoped_operation_registration* next_{nullptr};
    phase phase_{phase::unscoped};
    void* frame_target_{nullptr};
    void (*retire_cold_)(void*) noexcept {nullptr};
};

[[nodiscard]] void* task_frame_allocate(std::size_t bytes_value);
void task_frame_deallocate(void* pointer) noexcept;
[[nodiscard]] void* task_frame_allocate_aligned(std::size_t bytes_value, std::size_t alignment);

template <std::size_t t_alignment>
[[nodiscard]] void* task_frame_allocate(std::size_t bytes_value) {
    if constexpr (t_alignment > __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
        return task_frame_allocate_aligned(bytes_value, t_alignment);
    } else {
        return task_frame_allocate(bytes_value);
    }
}

template <typename t_type>
class task_promise;
template <typename t_type, typename handler_type>
class task_completion_state;

template <typename t_type>
concept asio_task_result = std::is_void_v<t_type> || std::is_nothrow_move_constructible_v<t_type>;

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
    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> coroutine) const noexcept {
        return coroutine.promise().control_.finish();
    }

    void await_resume() const noexcept {}
};

struct task_promise_pending final {};
struct task_promise_completed final {};

template <typename t_type>
struct task_promise_value final {
    t_type value_;
};

struct task_promise_failure final {
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
        state_.template emplace<completed_type>(result_type(std::forward<input_type>(value)));
    }

    void fail() noexcept {
        state_.template emplace<task_promise_failure>(std::current_exception());
    }

    result_type result() {
        if (const auto* failure = std::get_if<task_promise_failure>(&state_)) [[unlikely]] {
            std::rethrow_exception(failure->exception_);
        }
        if constexpr (std::is_void_v<result_type>) {
            assert(std::holds_alternative<completed_type>(state_));
        } else {
            auto* value = std::get_if<completed_type>(&state_);
            assert(value != nullptr);
            return std::move(value->value_);
        }
    }

private:
    using completed_type = std::conditional_t<std::is_void_v<result_type>, task_promise_completed, task_promise_value<result_type>>;
    std::variant<task_promise_pending, completed_type, task_promise_failure> state_;
};

// The coroutine protocol forbids declaring both return_value and return_void,
// so this base is the only part of the promise that differs between value and
// void tasks; every other promise member is stated once in task_promise.
template <typename t_type>
class task_promise_return {
public:
    template <typename u_type>
        requires std::constructible_from<t_type, u_type>
    void return_value(u_type&& value) noexcept(std::is_nothrow_constructible_v<t_type, u_type>) {
        state_.complete(std::forward<u_type>(value));
    }

protected:
    task_promise_result<t_type> state_;
};

template <>
class task_promise_return<void> {
public:
    void return_void() noexcept {
        state_.complete();
    }

protected:
    task_promise_result<void> state_;
};

template <typename t_type>
class task_promise final : public task_promise_return<t_type> {
public:
    static_assert(!std::is_reference_v<t_type>, "ruvia::task does not support reference result types");
    task_promise() noexcept = default;

    static void* operator new(std::size_t size) {
        return task_frame_allocate<alignof(task_promise)>(size);
    }
    static void operator delete(void* pointer, std::size_t) noexcept {
        task_frame_deallocate(pointer);
    }

    [[nodiscard]] task<t_type> get_return_object() noexcept;
    [[nodiscard]] std::suspend_always initial_suspend() const noexcept {
        return {};
    }
    [[nodiscard]] task_final_awaiter final_suspend() noexcept {
        return {};
    }

    void unhandled_exception() noexcept {
        this->state_.fail();
    }
    // No [[nodiscard]]: the attribute is not portable on the void return.
    t_type result() & {
        return this->state_.result();
    }

private:
    template <typename>
    friend class ruvia::task;
    template <typename>
    friend class ruvia::task_awaiter;
    template <typename, typename>
    friend class task_completion_state;
    friend class ruvia::task_scope;
    friend struct task_final_awaiter;

    task_frame_control control_;
};

}  // namespace detail
template <typename t_type, typename completion_token_type>
    requires detail::asio_task_result<t_type>
auto async_start(task<t_type>&& task_value, completion_token_type&& token);

// task is a structured, lazy coroutine owner. A cold task may be discarded and
// a completed task may be destroyed, but a started task must run to completion.
// Frame allocation happens when the coroutine is called; allocation failure
// throws std::bad_alloc before a task is returned.
// Over-aligned result types are supported. GCC 13/14 can misalign over-aligned
// coroutine locals independently of the promise allocator; keep such locals in
// separately allocated, owned storage rather than directly in the frame.
// Cancellation is cooperative: request it through the owning operation/scope
// and then await or join the task. Destroying a suspended frame would invalidate
// every external await registration that borrows it, so that contract violation
// terminates instead of manufacturing use-after-free cancellation semantics.
//
// T = void is served by this same template rather than by a specialization:
// task_promise<void> offers an identical promise interface, so nothing below
// depends on T. Stating the contract once is the point -- a duplicated copy of
// the terminate below and of the co_await deletions can drift out of step with
// this one, and the compiler would not say a word.
template <typename t_type>
class [[nodiscard]] task final {
public:
    using value_type = t_type;
    using promise_type = detail::task_promise<t_type>;
    using coroutine_type = std::coroutine_handle<promise_type>;

    task() = delete;

    task(const task&) = delete;
    task& operator=(const task&) = delete;

    task(task&& other) noexcept :
        coroutine_(std::exchange(other.coroutine_, {})),
        registration_(std::move(other.registration_), std::addressof(coroutine_)) {}
    task& operator=(task&&) = delete;

    ~task() {
        if (registration_.scoped()) {
            registration_.retire_frame();
        } else {
            reset();
        }
    }

    [[nodiscard]] task_awaiter<t_type> operator co_await() &&;
    [[nodiscard]] task_awaiter<t_type> operator co_await() & = delete;
    [[nodiscard]] task_awaiter<t_type> operator co_await() const& = delete;
    [[nodiscard]] task_awaiter<t_type> operator co_await() const&& = delete;

private:
    template <typename>
    friend class detail::task_promise;
    template <typename>
    friend class task_awaiter;
    template <typename, typename>
    friend class detail::task_completion_state;
    template <typename u_type, typename completion_token_type>
        requires detail::asio_task_result<u_type>
    friend auto async_start(task<u_type>&&, completion_token_type&&);
    friend class task_scope;
    template <typename u_type>
    friend task<u_type> make_scoped_operation(operation_scope&, task<u_type>);

    explicit task(coroutine_type coroutine) noexcept :
        coroutine_(coroutine) {}

    void prepare_direct_start() const {
        if (registration_.scoped()) {
            registration_.prepare_start();
        }
        if (coroutine_ == nullptr) {
            throw std::logic_error("cannot start an empty ruvia::task");
        }
    }

    void start() noexcept {
        if (registration_.scoped()) {
            registration_.start();
        }
        coroutine_.promise().control_.mark_started();
        coroutine_.resume();
    }

    void prepare_direct_completion() noexcept {
        if (registration_.scoped()) {
            registration_.prepare_completion();
        }
    }

    void finish_direct_completion() noexcept {
        if (registration_.scoped()) {
            reset();
            registration_.complete();
        }
    }

    static void retire_cold_frame(void* target) noexcept {
        auto& coroutine_slot = *static_cast<coroutine_type*>(target);
        if (coroutine_slot != nullptr) {
            auto coroutine = std::exchange(coroutine_slot, {});
            if (!coroutine.done() && coroutine.promise().control_.started()) {
                std::terminate();
            }
            coroutine.destroy();
        }
    }

    void reset() noexcept {
        retire_cold_frame(std::addressof(coroutine_));
    }

    void bind_scope(operation_scope& scope) noexcept {
        registration_.bind(scope, std::addressof(coroutine_), &task::retire_cold_frame);
    }

    coroutine_type coroutine_{};
    detail::scoped_operation_registration registration_{};
};

}  // namespace ruvia

namespace ruvia::detail {

template <typename t_type>
[[nodiscard]] task<t_type> task_promise<t_type>::get_return_object() noexcept {
    return task<t_type>{std::coroutine_handle<task_promise<t_type>>::from_promise(*this)};
}

}  // namespace ruvia::detail

namespace ruvia {

// The awaiter of `co_await std::move(task_value)`: it owns the started task
// for the duration of one await-expression and is neither movable nor
// addressable. Serves value-returning tasks and task<void> alike:
// task_promise<void>::result() returns void, so `return promise.result();`
// below is well-formed for it too. Keeping one
// awaiter keeps the empty-task check and the suspend sequence stated once.
//
// await_resume carries no [[nodiscard]]: the compiler calls it as part of the
// co_await expansion rather than callers, and the attribute is not portable on
// a void return type. task itself is [[nodiscard]], which is the check that
// catches a dropped task.
template <typename t_type>
class task_awaiter final {
public:
    task_awaiter(const task_awaiter&) = delete;
    task_awaiter& operator=(const task_awaiter&) = delete;
    task_awaiter(task_awaiter&&) = delete;
    task_awaiter& operator=(task_awaiter&&) = delete;
    void operator&() const = delete;

    [[nodiscard]] bool await_ready() const noexcept {
        return task_.coroutine_.done();
    }

    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept {
        task_.coroutine_.promise().control_.set_continuation(continuation);
        task_.coroutine_.promise().control_.mark_started();
        return task_.coroutine_;
    }

    t_type await_resume() {
        task_.prepare_direct_completion();
        struct complete_guard final {
            task_awaiter& awaiter_;
            ~complete_guard() {
                awaiter_.task_.finish_direct_completion();
            }
        } complete_value{*this};
        return task_.coroutine_.promise().result();
    }

private:
    friend class task<t_type>;

    explicit task_awaiter(task<t_type>&& task_value) :
        task_(take_ready_to_start(std::move(task_value))) {
        // Enter running only after the final owner has received the cold task;
        // optional return-value elision must not decide whether a move is legal.
        if (task_.registration_.scoped()) {
            task_.registration_.start();
        }
    }

    [[nodiscard]] static task<t_type> take_ready_to_start(task<t_type>&& task_value) {
        task_value.prepare_direct_start();
        return std::move(task_value);
    }

    task<t_type> task_;
};

template <typename t_type>
[[nodiscard]] task_awaiter<t_type> task<t_type>::operator co_await() && {
    return task_awaiter<t_type>{std::move(*this)};
}

// Collects the calling thread's mimalloc heap and forces the shared arenas to
// purge. Live allocations are untouched. A completely idle thread never runs
// mimalloc's deferred collection by itself, so a worker that went quiet calls
// this once.
void collect_thread_heap() noexcept;

}  // namespace ruvia
