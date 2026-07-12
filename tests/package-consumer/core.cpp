#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <utility>

#include <ruvia/core/Task.h>
#include <ruvia/core/detail/PoolWaiterQueue.h>
#include <ruvia/core/memory/MemoryPool.h>
#include <ruvia/core/memory/PmrObject.h>

template <typename T>
concept HasPoolWaiterIndex = requires(const T& value) {
    { value.index() } -> std::same_as<std::size_t>;
};

template <typename T>
concept AcceptsLoosePoolWaiterTuple = requires(
    bool& ready,
    bool& timedOut,
    std::size_t& index) {
    T(
        ready,
        timedOut,
        index,
        std::chrono::steady_clock::time_point{});
};

template <typename T>
concept HasLoosePoolWaiterBind = requires(
    T& waiter,
    bool& ready,
    bool& timedOut,
    std::size_t& index) {
    waiter.bind(
        ready,
        timedOut,
        index,
        std::chrono::steady_clock::time_point{},
        std::coroutine_handle<>{});
};

template <typename T>
concept AcceptsPoolCloseSentinel = requires(T& queue) {
    queue.closeAll(std::size_t{});
};

template <typename T>
concept HasParallelPoolWaiterResultAccessor = requires(const T& waiter) {
    waiter.result();
};

static_assert(!std::default_initializable<ruvia::detail::PoolWaiter>);
static_assert(std::constructible_from<
    ruvia::detail::PoolWaiter,
    std::chrono::steady_clock::time_point>);
static_assert(!AcceptsLoosePoolWaiterTuple<ruvia::detail::PoolWaiter>);
static_assert(!HasLoosePoolWaiterBind<ruvia::detail::PoolWaiter>);
static_assert(!HasParallelPoolWaiterResultAccessor<
    ruvia::detail::PoolWaiter>);
static_assert(!std::default_initializable<ruvia::detail::PoolWaiterResult>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::PoolWaiterResult&>()
                 .acquired()),
    const ruvia::detail::PoolWaiterAcquired*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::PoolWaiterResult&>()
                 .timedOut()),
    const ruvia::detail::PoolWaiterTimedOut*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::PoolWaiterResult&>()
                 .closed()),
    const ruvia::detail::PoolWaiterClosed*>);
static_assert(!HasPoolWaiterIndex<ruvia::detail::PoolWaiterResult>);
static_assert(HasPoolWaiterIndex<ruvia::detail::PoolWaiterAcquired>);
static_assert(!HasPoolWaiterIndex<ruvia::detail::PoolWaiterTimedOut>);
static_assert(!HasPoolWaiterIndex<ruvia::detail::PoolWaiterClosed>);
static_assert(!std::constructible_from<
    ruvia::detail::PoolWaiterAcquired,
    std::size_t>);
static_assert(!std::default_initializable<ruvia::detail::PoolWaiterTimedOut>);
static_assert(!std::default_initializable<ruvia::detail::PoolWaiterClosed>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::PoolWaiter&>().await_resume()),
    const ruvia::detail::PoolWaiterResult&>);
static_assert(std::same_as<
    decltype(&ruvia::detail::PoolWaiterQueue::closeAll),
    void (ruvia::detail::PoolWaiterQueue::*)() noexcept>);
static_assert(!AcceptsPoolCloseSentinel<ruvia::detail::PoolWaiterQueue>);

int main() {
    ruvia::WorkerMemory worker;
    return worker.resource() == nullptr ? 1 : 0;
}
