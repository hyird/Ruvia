#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <optional>
#include <utility>

#include <ruvia/core/Task.h>
#include <ruvia/core/Channel.h>
#include <ruvia/core/OneShot.h>
#include <ruvia/core/WorkerWaitResult.h>
#include <ruvia/core/detail/ConnectionScanner.h>
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

template <typename Options>
concept HasConnectionTimeoutMillisecondSentinels = requires(Options& options) {
    options.idleTimeoutMs;
    options.initialReadTimeoutMs;
    options.payloadReadTimeoutMs;
    options.writeTimeoutMs;
};

template <typename T>
concept HasPublicWorkerWaitFields = requires(T& result) {
    result.status;
    result.value;
};

template <typename T>
concept HasAnyRvalueWorkerWaitAccessor =
    requires(T&& result) { std::move(result).value(); } ||
    requires(T&& result) { std::move(result).closed(); } ||
    requires(T&& result) { std::move(result).workerStopping(); } ||
    requires(T&& result) { std::move(result).timedOut(); };

template <typename T>
concept HasAnyRvaluePoolWaiterAccessor =
    requires(T&& result) { std::move(result).acquired(); } ||
    requires(T&& result) { std::move(result).timedOut(); } ||
    requires(T&& result) { std::move(result).closed(); };

static_assert(!HasPublicWorkerWaitFields<ruvia::WorkerWaitResult<int>>);
static_assert(!HasAnyRvalueWorkerWaitAccessor<
    ruvia::WorkerWaitResult<int>>);
static_assert(!std::default_initializable<ruvia::WorkerWaitResult<int>>);
static_assert(!std::default_initializable<ruvia::WorkerWaitClosed>);
static_assert(!std::default_initializable<ruvia::WorkerWaitStopping>);
static_assert(!std::default_initializable<ruvia::WorkerWaitTimedOut>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::WorkerWaitResult<int>&>().value()),
    const int*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::WorkerWaitResult<int>&>().closed()),
    const ruvia::WorkerWaitClosed*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::WorkerWaitResult<int>&>().workerStopping()),
    const ruvia::WorkerWaitStopping*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::WorkerWaitResult<int>&>().timedOut()),
    const ruvia::WorkerWaitTimedOut*>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::ChannelReceiver<int>&>().receive()),
    ruvia::Task<ruvia::WorkerWaitResult<int>>>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::OneShotReceiver<int>&>().wait()),
    ruvia::Task<ruvia::WorkerWaitResult<int>>>);
static_assert(!std::default_initializable<ruvia::ChannelReceiver<int>>);
static_assert(std::move_constructible<ruvia::ChannelReceiver<int>>);
static_assert(!std::assignable_from<
    ruvia::ChannelReceiver<int>&,
    ruvia::ChannelReceiver<int>&&>);
static_assert(!std::default_initializable<ruvia::OneShotReceiver<int>>);
static_assert(std::move_constructible<ruvia::OneShotReceiver<int>>);
static_assert(!std::assignable_from<
    ruvia::OneShotReceiver<int>&,
    ruvia::OneShotReceiver<int>&&>);
static_assert(std::move_constructible<ruvia::Task<void>>);
static_assert(!std::assignable_from<ruvia::Task<void>&, ruvia::Task<void>&&>);
static_assert(std::move_constructible<ruvia::Task<int>>);
static_assert(!std::assignable_from<ruvia::Task<int>&, ruvia::Task<int>&&>);

static_assert(!HasConnectionTimeoutMillisecondSentinels<
              ruvia::detail::ConnectionScannerOptions>);
static_assert(std::same_as<
              decltype(ruvia::detail::ConnectionScannerOptions{}.idleTimeout),
              std::optional<std::chrono::milliseconds>>);
static_assert(std::same_as<
              decltype(ruvia::detail::ConnectionScannerOptions{}.initialReadTimeout),
              std::optional<std::chrono::milliseconds>>);
static_assert(std::same_as<
              decltype(ruvia::detail::ConnectionScannerOptions{}.payloadReadTimeout),
              std::optional<std::chrono::milliseconds>>);
static_assert(std::same_as<
              decltype(ruvia::detail::ConnectionScannerOptions{}.writeTimeout),
              std::optional<std::chrono::milliseconds>>);

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
static_assert(!HasAnyRvaluePoolWaiterAccessor<
    ruvia::detail::PoolWaiterResult>);
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
