#pragma once

#include "ruvia/app/Task.h"

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
        const void* table{nullptr};
        const void* route{nullptr};
        Context* context{nullptr};
        void* outcome{nullptr};
        std::size_t index{0};
        bool repeated{false};
    };

    using Invoke = Task<void> (*)(State);

    class Awaitable final {
    public:
        Awaitable(const Awaitable&) = delete;
        Awaitable& operator=(const Awaitable&) = delete;
        Awaitable(Awaitable&&) = delete;
        Awaitable& operator=(Awaitable&&) = delete;

        [[nodiscard]] auto operator co_await() && {
            return std::move(task_).operator co_await();
        }
        [[nodiscard]] auto operator co_await() & = delete;
        [[nodiscard]] auto operator co_await() const& = delete;
        [[nodiscard]] auto operator co_await() const&& = delete;

    private:
        friend class Next;

        explicit Awaitable(Task<void>&& task) noexcept
            : task_(std::move(task)) {}

        Task<void> task_;
    };

    Next(const Next&) = delete;
    Next& operator=(const Next&) = delete;
    Next(Next&&) = delete;
    Next& operator=(Next&&) = delete;

    [[nodiscard]] Awaitable operator()() const;

private:
    friend struct detail::NextAccess;

    constexpr Next(State state, Invoke invoke) noexcept
        : state_(state),
          invoke_(invoke) {}

    State state_;
    Invoke invoke_{nullptr};
    mutable bool invoked_{false};
};

}  // namespace ruvia
