#pragma once

#include "ruvia/app/Task.h"

#include <coroutine>
#include <cstddef>
#include <utility>

namespace ruvia {

class Context;

namespace detail {
struct NextAccess;
}  // namespace detail

class Next final {
public:
    struct State final {
        struct Control final {
            bool invoked{false};
            bool active{true};
        };

        const void* table{nullptr};
        const void* route{nullptr};
        Context* context{nullptr};
        void* outcome{nullptr};
        Control* control{nullptr};
        std::size_t index{0};
        bool repeated{false};
    };

    using Invoke = Task<void> (*)(State);

    class Awaitable final {
        class Awaiter final {
        public:
            Awaiter(const Awaiter&) = delete;
            Awaiter& operator=(const Awaiter&) = delete;
            Awaiter(Awaiter&&) = delete;
            Awaiter& operator=(Awaiter&&) = delete;

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

            const Awaiter* operator&() const = delete;
            Awaiter* operator&() = delete;

        private:
            friend class Awaitable;

            explicit Awaiter(Task<void>&& task)
                : awaiter_(std::move(task).operator co_await()) {}

            detail::TaskAwaiter<void> awaiter_;
        };

    public:
        Awaitable(const Awaitable&) = delete;
        Awaitable& operator=(const Awaitable&) = delete;
        Awaitable(Awaitable&&) = delete;
        Awaitable& operator=(Awaitable&&) = delete;

        [[nodiscard]] Awaiter operator co_await() && {
            auto state = state_;
            state.repeated = state.repeated || awaited_;
            awaited_ = true;
            return Awaiter(invoke_(state));
        }
        [[nodiscard]] auto operator co_await() & = delete;
        [[nodiscard]] auto operator co_await() const& = delete;
        [[nodiscard]] auto operator co_await() const&& = delete;
        const Awaitable* operator&() const = delete;
        Awaitable* operator&() = delete;

    private:
        friend class Next;

        constexpr Awaitable(State state, Invoke invoke) noexcept
            : state_(state),
              invoke_(invoke) {}

        State state_;
        Invoke invoke_{nullptr};
        bool awaited_{false};
    };

    Next(const Next&) = delete;
    Next& operator=(const Next&) = delete;
    Next(Next&&) = delete;
    Next& operator=(Next&&) = delete;

    [[nodiscard]] Awaitable operator()() &;
    [[nodiscard]] Awaitable operator()() const& = delete;
    [[nodiscard]] Awaitable operator()() && = delete;
    [[nodiscard]] Awaitable operator()() const&& = delete;
    const Next* operator&() const = delete;
    Next* operator&() = delete;

private:
    friend struct detail::NextAccess;

    constexpr Next(State state, Invoke invoke) noexcept
        : state_(state),
          invoke_(invoke) {}

    State state_;
    Invoke invoke_{nullptr};
};

}  // namespace ruvia
