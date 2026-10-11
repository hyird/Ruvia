#pragma once

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "ruvia/core/task.h"

namespace ruvia {

class context;

namespace detail {
struct next_access;
struct middleware_chain;

struct next_state final {
    enum class invocation_type : std::uint8_t {
        ready,
        repeated,
        expired,
    };

    struct control_type final {
        enum class phase_type : std::uint8_t {
            fresh,
            invoked,
            expired,
        };

        [[nodiscard]] invocation_type begin_invocation() noexcept {
            if (phase_ == phase_type::fresh) {
                phase_ = phase_type::invoked;
                return invocation_type::ready;
            }
            return phase_ == phase_type::invoked ? invocation_type::repeated : invocation_type::expired;
        }

        void expire() noexcept {
            phase_ = phase_type::expired;
        }

    private:
        phase_type phase_{phase_type::fresh};
    };

    // The chain being walked, the request it runs for, this hop's single-shot
    // control block, and the frame index the continuation resumes at.
    const middleware_chain* chain_{nullptr};
    context* context_{nullptr};
    control_type* control_{nullptr};
    std::size_t index_{0};
    invocation_type invocation_{invocation_type::ready};
};

using next_invoke_type = task<void> (*)(next_state);
}  // namespace detail

class next final {
public:
    class awaitable_type final {
        class awaiter_type final {
        public:
            awaiter_type(const awaiter_type&) = delete;
            awaiter_type& operator=(const awaiter_type&) = delete;
            awaiter_type(awaiter_type&&) = delete;
            awaiter_type& operator=(awaiter_type&&) = delete;

            [[nodiscard]] bool await_ready() const noexcept {
                return awaiter_.await_ready();
            }

            [[nodiscard]] std::coroutine_handle<> await_suspend(
                std::coroutine_handle<> continuation) noexcept {
                return awaiter_.await_suspend(continuation);
            }

            void await_resume() {
                awaiter_.await_resume();
            }

            const awaiter_type* operator&() const = delete;
            awaiter_type* operator&() = delete;

        private:
            friend class awaitable_type;

            explicit awaiter_type(task<void>&& task_value)
                : awaiter_(std::move(task_value).operator co_await()) {}

            detail::task_awaiter<void> awaiter_;
        };

    public:
        awaitable_type(const awaitable_type&) = delete;
        awaitable_type& operator=(const awaitable_type&) = delete;
        awaitable_type(awaitable_type&&) = delete;
        awaitable_type& operator=(awaitable_type&&) = delete;

        [[nodiscard]] awaiter_type operator co_await() && {
            auto state_value = state_;
            if (phase_ == phase_type::awaited) {
                state_value.invocation_ = detail::next_state::invocation_type::repeated;
            }
            phase_ = phase_type::awaited;
            return awaiter_type(invoke_(state_value));
        }
        [[nodiscard]] auto operator co_await() & = delete;
        [[nodiscard]] auto operator co_await() const& = delete;
        [[nodiscard]] auto operator co_await() const&& = delete;
        const awaitable_type* operator&() const = delete;
        awaitable_type* operator&() = delete;

    private:
        friend class next;

        constexpr awaitable_type(detail::next_state state_value, detail::next_invoke_type invoke) noexcept
            : state_(state_value),
              invoke_(invoke) {}

        detail::next_state state_;
        detail::next_invoke_type invoke_{nullptr};
        enum class phase_type : std::uint8_t {
            fresh,
            awaited,
        };
        phase_type phase_{phase_type::fresh};
    };

    next(const next&) = delete;
    next& operator=(const next&) = delete;
    next(next&&) = delete;
    next& operator=(next&&) = delete;

    [[nodiscard]] awaitable_type operator()() &;
    [[nodiscard]] awaitable_type operator()() const& = delete;
    [[nodiscard]] awaitable_type operator()() && = delete;
    [[nodiscard]] awaitable_type operator()() const&& = delete;
    const next* operator&() const = delete;
    next* operator&() = delete;

private:
    friend struct detail::next_access;

    constexpr next(detail::next_state state_value, detail::next_invoke_type invoke) noexcept
        : state_(state_value),
          invoke_(invoke) {}

    detail::next_state state_;
    detail::next_invoke_type invoke_{nullptr};
};

}  // namespace ruvia
