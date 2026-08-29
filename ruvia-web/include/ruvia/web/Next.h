#pragma once

#include "ruvia/core/Task.h"

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ruvia {

class Context;

namespace detail {
template <typename Result, typename... Args>
class CallableRef;
struct NextAccess;
class RouteEntry;
class RouteTable;
class StreamMiddlewareChainState;

struct NextState final {
    enum class Invocation : std::uint8_t {
        kReady,
        kRepeated,
        kExpired,
    };

    struct Control final {
        enum class Phase : std::uint8_t {
            kFresh,
            kInvoked,
            kExpired,
        };

        [[nodiscard]] Invocation beginInvocation() noexcept {
            if (phase_ == Phase::kFresh) {
                phase_ = Phase::kInvoked;
                return Invocation::kReady;
            }
            return phase_ == Phase::kInvoked ? Invocation::kRepeated : Invocation::kExpired;
        }

        void expire() noexcept {
            phase_ = Phase::kExpired;
        }

        [[nodiscard]] Phase phase() const noexcept {
            return phase_;
        }

    private:
        Phase phase_{Phase::kFresh};
    };

    const RouteTable* table{nullptr};
    const RouteEntry* route{nullptr};
    Context* context{nullptr};
    StreamMiddlewareChainState* streamChain{nullptr};
    const CallableRef<void, Context&>* streamHandler{nullptr};
    // Set only for the unmatched-request chain, whose terminal is the
    // 404/405/501 response rather than a route endpoint.
    const void* unmatchedTerminal{nullptr};
    Control* control{nullptr};
    std::size_t index{0};
    Invocation invocation{Invocation::kReady};
};

using NextInvoke = Task<void> (*)(NextState);
}  // namespace detail

class Next final {
public:
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
            if (phase_ == Phase::kAwaited) {
                state.invocation = detail::NextState::Invocation::kRepeated;
            }
            phase_ = Phase::kAwaited;
            return Awaiter(invoke_(state));
        }
        [[nodiscard]] auto operator co_await() & = delete;
        [[nodiscard]] auto operator co_await() const& = delete;
        [[nodiscard]] auto operator co_await() const&& = delete;
        const Awaitable* operator&() const = delete;
        Awaitable* operator&() = delete;

    private:
        friend class Next;

        constexpr Awaitable(detail::NextState state, detail::NextInvoke invoke) noexcept
            : state_(state),
              invoke_(invoke) {}

        detail::NextState state_;
        detail::NextInvoke invoke_{nullptr};
        enum class Phase : std::uint8_t {
            kFresh,
            kAwaited,
        };
        Phase phase_{Phase::kFresh};
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

    constexpr Next(detail::NextState state, detail::NextInvoke invoke) noexcept
        : state_(state),
          invoke_(invoke) {}

    detail::NextState state_;
    detail::NextInvoke invoke_{nullptr};
};

}  // namespace ruvia
