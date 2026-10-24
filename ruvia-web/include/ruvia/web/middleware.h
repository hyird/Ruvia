#pragma once

#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "ruvia/core/task.h"
#include "ruvia/http/borrowed_text.h"

namespace ruvia {

class context;
class http_response;

struct middleware_scope_options final {
    borrowed_text prefix_{};
};

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
    class awaitable final {
        class awaiter final {
        public:
            awaiter(const awaiter&) = delete;
            awaiter& operator=(const awaiter&) = delete;
            awaiter(awaiter&&) = delete;
            awaiter& operator=(awaiter&&) = delete;

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

            const awaiter* operator&() const = delete;
            awaiter* operator&() = delete;

        private:
            friend class awaitable;

            explicit awaiter(task<void>&& task_value)
                : awaiter_(std::move(task_value).operator co_await()) {}

            detail::task_awaiter<void> awaiter_;
        };

    public:
        awaitable(const awaitable&) = delete;
        awaitable& operator=(const awaitable&) = delete;
        awaitable(awaitable&&) = delete;
        awaitable& operator=(awaitable&&) = delete;

        [[nodiscard]] awaiter operator co_await() && {
            auto state_value = state_;
            if (phase_ == phase_type::awaited) {
                state_value.invocation_ = detail::next_state::invocation_type::repeated;
            }
            phase_ = phase_type::awaited;
            return awaiter(invoke_(state_value));
        }
        [[nodiscard]] auto operator co_await() & = delete;
        [[nodiscard]] auto operator co_await() const& = delete;
        [[nodiscard]] auto operator co_await() const&& = delete;
        const awaitable* operator&() const = delete;
        awaitable* operator&() = delete;

    private:
        friend class next;

        constexpr awaitable(detail::next_state state_value, detail::next_invoke_type invoke) noexcept
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

    [[nodiscard]] awaitable operator()() &;
    [[nodiscard]] awaitable operator()() const& = delete;
    [[nodiscard]] awaitable operator()() && = delete;
    [[nodiscard]] awaitable operator()() const&& = delete;
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
namespace detail {

template <typename middleware_type>
concept void_handle_middleware =
    requires { static_cast<task<void> (middleware_type::*)(context&, next&)>(&middleware_type::handle); };

template <typename middleware_type>
concept response_handle_middleware = requires {
    static_cast<task<http_response> (middleware_type::*)(context&, next&)>(&middleware_type::handle);
};

}  // namespace detail

// A middleware is any final class with a public
//     task<void> handle(context&, next&)
// or
//     task<http_response> handle(context&, next&)
// member. It derives from nothing: the framework composes it through per-type
// entry points and reads its route properties from static members.
//
// application::use<T>() instantiates the descriptor at the call site. Built-in
// middleware headers already include that machinery. A custom middleware's
// use<T>() call site needs controller.h, testing.h, or
// detail/middleware/middleware_registration.h. This header stays free of
// context so app.h does not pull it in.
template <typename middleware_type>
concept middleware = std::is_class_v<middleware_type> && std::is_final_v<middleware_type> &&
                     (detail::void_handle_middleware<middleware_type> ||
                         detail::response_handle_middleware<middleware_type>);

template <typename policy_type>
concept route_policy =
    std::is_class_v<policy_type> && std::is_final_v<policy_type> && !middleware<policy_type> &&
    (requires {
        { policy_type::ruvia_request_body_limit_bytes } -> std::convertible_to<std::size_t>; } || requires {
            { policy_type::ruvia_stream_request_body } -> std::convertible_to<bool>; });

template <typename modifier_type>
concept route_modifier = middleware<modifier_type> || route_policy<modifier_type>;

}  // namespace ruvia
