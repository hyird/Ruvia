#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

#include <ruvia/core/version.h>
#include <ruvia/core/Task.h>
#include <ruvia/core/EventLoopPool.h>
#include <ruvia/core/TaskScope.h>
#include <ruvia/core/Channel.h>
#include <ruvia/core/OneShot.h>
#include <ruvia/core/WorkerWaitResult.h>
#include <ruvia/core/detail/AsioAwait.h>
#include <ruvia/core/detail/ConnectionScanner.h>
#include <ruvia/core/detail/OperationDeadline.h>
#include <ruvia/core/detail/PoolLeaseScheduler.h>
#include <ruvia/core/detail/PoolWaiterQueue.h>
#include <ruvia/core/detail/WorkerTimer.h>
#include <ruvia/core/detail/WorkerWaitAwaiter.h>
#include <ruvia/core/memory/MemoryPool.h>
#include <ruvia/core/memory/PmrObject.h>

#ifdef RUVIA_EXPECTED_VERSION_MAJOR
static_assert(RUVIA_VERSION_MAJOR == RUVIA_EXPECTED_VERSION_MAJOR);
static_assert(RUVIA_VERSION_MINOR == RUVIA_EXPECTED_VERSION_MINOR);
static_assert(RUVIA_VERSION_PATCH == RUVIA_EXPECTED_VERSION_PATCH);
static_assert(
    std::string_view(RUVIA_VERSION_STRING) ==
    std::string_view(RUVIA_EXPECTED_VERSION_STRING));
#endif

template <typename T>
concept HasPoolWaiterIndex = requires(const T& value) {
    { value.index() } -> std::same_as<std::size_t>;
};

template <typename T>
concept HasErasedArenaEmplace = requires(T& memory) {
    memory.template emplace<int>(1);
};

template <typename T>
concept ExposesRvalueRequestMemoryBorrow =
    requires(T&& memory) { std::move(memory).resource(); } ||
    requires(T&& memory) { std::move(memory).template allocator<>(); };

static_assert(!HasErasedArenaEmplace<ruvia::RequestMemory>);
static_assert(!ExposesRvalueRequestMemoryBorrow<ruvia::RequestMemory>);

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

template <typename Entry>
concept HasTargetOnlyPeriodicCheck = requires(Entry& entry, void* target) {
    entry.setPeriodicCheck(
        target,
        static_cast<ruvia::detail::ConnectionScanner::PeriodicCheck>(nullptr));
};

template <typename Scanner>
concept HasTargetOnlyWorkerScanner = requires(Scanner& scanner, void* target) {
    scanner.setWorkerScanner(target, static_cast<void (*)(void*) noexcept>(nullptr));
};

template <typename T>
concept HasPublicWorkerWaitFields = requires(T& result) {
    result.status;
    result.value;
};

template <typename T>
concept HasLooseTaskCompletionFields = requires(T& result) {
    result.exception;
    result.value;
};

template <typename T>
concept HasAnyRvalueTaskCompletionAccessor =
    requires(T&& result) { std::move(result).success(); } ||
    requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasRvalueAsioCompletionResult =
    requires(T&& result) { std::move(result).result(); };

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

enum class PackageDeadlineKind {
    kRead
};

template <typename T>
concept HasRvalueOperationDeadlineKind =
    requires(T&& deadline) { std::move(deadline).kind(); };

using PackageOperationDeadline =
    ruvia::detail::OperationDeadline<PackageDeadlineKind>;

static_assert(std::default_initializable<PackageOperationDeadline>);
static_assert(std::same_as<
    decltype(std::declval<const PackageOperationDeadline&>().kind()),
    const PackageDeadlineKind*>);
static_assert(std::same_as<
    decltype(std::declval<PackageOperationDeadline&>().expire(
        std::chrono::steady_clock::time_point{})),
    std::optional<PackageDeadlineKind>>);
static_assert(!HasRvalueOperationDeadlineKind<PackageOperationDeadline>);

static_assert(!HasPublicWorkerWaitFields<ruvia::WorkerWaitResult<int>>);
static_assert(!HasLooseTaskCompletionFields<
              ruvia::detail::TaskCompletionResult<int>>);
static_assert(!HasAnyRvalueTaskCompletionAccessor<
              ruvia::detail::TaskCompletionResult<int>>);
static_assert(!HasAnyRvalueTaskCompletionAccessor<
              ruvia::detail::TaskCompletionResult<void>>);
static_assert(!std::default_initializable<
              ruvia::detail::TaskCompletionResult<int>>);
static_assert(!std::default_initializable<
              ruvia::detail::TaskCompletionResult<void>>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::
        TaskCompletionResult<int>&>().success()),
    ruvia::detail::TaskCompletionSuccess<int>*>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        TaskCompletionResult<int>&>().failure()),
    const ruvia::detail::TaskCompletionFailure*>);
static_assert(!std::default_initializable<
              ruvia::detail::AsioCompletion<std::size_t>>);
static_assert(!std::default_initializable<
              ruvia::detail::AsioCompletion<void>>);
static_assert(!HasRvalueAsioCompletionResult<
              ruvia::detail::AsioCompletion<std::size_t>>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        AsioCompletion<std::size_t>&>().errorCode()),
    std::error_code>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::
        AsioCompletion<std::size_t>&>().result()),
    const std::size_t&>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::
        AsioCompletion<std::size_t>&&>().takeResult()),
    std::size_t>);
static_assert(!HasAnyRvalueWorkerWaitAccessor<
    ruvia::WorkerWaitResult<int>>);
static_assert(!std::convertible_to<
              ruvia::detail::WorkerTimerOutcome, bool>);
static_assert(std::default_initializable<
              ruvia::detail::WorkerWaitAwaitState<int>>);
static_assert(!std::copy_constructible<
              ruvia::detail::WorkerWaitAwaitState<int>>);
static_assert(!std::move_constructible<
              ruvia::detail::WorkerWaitAwaitState<int>>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::WorkerWaitAwaitState<int>&>().suspend(
        std::coroutine_handle<>{})),
    bool>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::WorkerWaitAwaitState<int>&>().complete(
        std::declval<ruvia::WorkerWaitResult<int>>())),
    bool>);
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
    decltype(std::declval<ruvia::ChannelReceiver<int>&>().receiveFor(
        std::chrono::milliseconds(1))),
    ruvia::Task<ruvia::WorkerWaitResult<int>>>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::OneShotReceiver<int>&>().wait()),
    ruvia::Task<ruvia::WorkerWaitResult<int>>>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::OneShotReceiver<int>&>().waitFor(
        std::chrono::milliseconds(1))),
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
static_assert(!std::default_initializable<ruvia::TaskScope>);
static_assert(!std::copy_constructible<ruvia::TaskScope>);
static_assert(!std::move_constructible<ruvia::TaskScope>);
static_assert(std::default_initializable<ruvia::EventLoop>);
static_assert(std::copy_constructible<ruvia::EventLoop>);
static_assert(!std::default_initializable<ruvia::EventLoopAttachment>);
static_assert(!std::copy_constructible<ruvia::EventLoopAttachment>);
static_assert(std::move_constructible<ruvia::EventLoopAttachment>);
static_assert(!std::assignable_from<
    ruvia::EventLoopAttachment&,
    ruvia::EventLoopAttachment&&>);
static_assert(!std::copy_constructible<ruvia::EventLoopStopRegistration>);
static_assert(std::move_constructible<ruvia::EventLoopStopRegistration>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::EventLoop&>().ioContext()),
    asio::io_context&>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::EventLoop&>().executor()),
    asio::io_context::executor_type>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::EventLoop&>().handle()),
    ruvia::WorkerHandle>);
static_assert(std::same_as<
    decltype(ruvia::attachEventLoop(std::declval<asio::io_context&>())),
    ruvia::EventLoopAttachment>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::EventLoopAttachment&>().loop()),
    ruvia::EventLoop>);

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
using ScannerRegistration =
    ruvia::detail::ConnectionScanner::PeriodicCheckRegistration;
static_assert(std::same_as<
    ruvia::detail::ConnectionScanner::PeriodicCheck,
    void (*)(void*, std::int64_t) noexcept>);
static_assert(std::default_initializable<ScannerRegistration>);
static_assert(!std::movable<ScannerRegistration>);
static_assert(!HasTargetOnlyPeriodicCheck<
              ruvia::detail::ConnectionScanner::Entry>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::ConnectionScanner::Entry&>()
                 .registerPeriodicCheck(
                     std::declval<ScannerRegistration&>(),
                     nullptr,
                     static_cast<
                         ruvia::detail::ConnectionScanner::PeriodicCheck>(nullptr))),
    void>);
using WorkerMaintenanceRegistration =
    ruvia::detail::ConnectionScanner::WorkerMaintenanceRegistration;
static_assert(std::default_initializable<WorkerMaintenanceRegistration>);
static_assert(!std::movable<WorkerMaintenanceRegistration>);
static_assert(!HasTargetOnlyWorkerScanner<
              ruvia::detail::ConnectionScanner>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::ConnectionScanner&>()
                 .registerWorkerMaintenance(
                     std::declval<WorkerMaintenanceRegistration&>(),
                     nullptr,
                     static_cast<
                         ruvia::detail::ConnectionScanner::
                             WorkerMaintenanceCheck>(nullptr))),
    void>);

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
static_assert(!std::copyable<ruvia::detail::PoolLeaseScheduler>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::PoolLeaseScheduler&>().acquire(
        std::optional<std::chrono::milliseconds>{})),
    ruvia::Task<ruvia::detail::PoolWaiterResult>>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::PoolLeaseScheduler&>().close()),
    bool>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::detail::PoolLeaseScheduler&>().release(
        std::size_t{})),
    ruvia::detail::PoolLeaseReleaseStatus>);

int main() {
    ruvia::WorkerMemory worker;
    return worker.resource() == nullptr ? 1 : 0;
}
