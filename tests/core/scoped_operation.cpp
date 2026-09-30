#include <atomic>
#include <future>
#include <limits>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <utility>

#include "ruvia/core/EventLoopPool.h"
#include "ruvia/core/ScopedOperation.h"
#include "ruvia/core/detail/worker/WorkerSignal.h"

#include "test_harness.h"

namespace {

class CountingResource final : public std::pmr::memory_resource {
public:
    std::size_t inUseAllocations{};
    std::size_t inUseBytes{};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        auto* allocation = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++inUseAllocations;
        inUseBytes += bytes;
        return allocation;
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        --inUseAllocations;
        inUseBytes -= bytes;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

class TestScopedCapability final : private ruvia::detail::ScopedCapabilityNode {
public:
    TestScopedCapability(ruvia::detail::ScopedOperationScope& scope, int& expiredCount,
        CountingResource* resource = nullptr, std::size_t* bytesAtExpire = nullptr,
        const bool* leaseActive = nullptr, bool* leaseAtExpire = nullptr) noexcept
        : ScopedCapabilityNode(scope, &TestScopedCapability::expire),
          expiredCount_(&expiredCount),
          resource_(resource),
          bytesAtExpire_(bytesAtExpire),
          leaseActive_(leaseActive),
          leaseAtExpire_(leaseAtExpire) {}

private:
    static void expire(ruvia::detail::ScopedCapabilityNode& node) noexcept {
        auto& capability = static_cast<TestScopedCapability&>(node);
        ++*capability.expiredCount_;
        if (capability.resource_ != nullptr && capability.bytesAtExpire_ != nullptr) {
            *capability.bytesAtExpire_ = capability.resource_->inUseBytes;
        }
        if (capability.leaseActive_ != nullptr && capability.leaseAtExpire_ != nullptr) {
            *capability.leaseAtExpire_ = *capability.leaseActive_;
        }
    }

    int* expiredCount_;
    CountingResource* resource_;
    std::size_t* bytesAtExpire_;
    const bool* leaseActive_;
    bool* leaseAtExpire_;
};

ruvia::Task<void> awaitScopedOperation(ruvia::ScopedOperation<void>& operation) {
    co_await std::move(operation);
}

ruvia::Task<void> coldWithPayload(std::pmr::string payload) {
    if (payload.empty()) {
        throw std::runtime_error("payload unexpectedly empty");
    }
    co_return;
}

class FrameLease final {
public:
    explicit FrameLease(bool& active) noexcept
        : active_(&active) {
        *active_ = true;
    }
    FrameLease(const FrameLease&) = delete;
    FrameLease& operator=(const FrameLease&) = delete;
    FrameLease(FrameLease&& other) noexcept
        : active_(std::exchange(other.active_, nullptr)) {}
    FrameLease& operator=(FrameLease&&) = delete;
    ~FrameLease() {
        if (active_ != nullptr) {
            *active_ = false;
        }
    }

private:
    bool* active_;
};

ruvia::Task<std::size_t> joinAndObserveBytes(
    ruvia::detail::ScopedOperationScope& scope, const CountingResource& resource) {
    co_await scope.closeAndJoin();
    co_return resource.inUseBytes;
}

ruvia::Task<void> waitWithPayload(std::pmr::string payload,
    ruvia::detail::WorkerSignal& signal, std::promise<void>& started, FrameLease lease) {
    static_cast<void>(lease);
    started.set_value();
    co_await signal.wait();
    if (payload.empty()) {
        throw std::runtime_error("payload unexpectedly empty");
    }
}

ruvia::Task<void> waitThenThrow(std::pmr::string payload,
    ruvia::detail::WorkerSignal& signal, std::promise<void>& started) {
    started.set_value();
    co_await signal.wait();
    if (!payload.empty()) {
        throw std::runtime_error("operation failure");
    }
}

ruvia::Task<bool> waitThenObserveCancellation(std::pmr::string payload,
    ruvia::detail::WorkerSignal& signal, std::promise<void>& started,
    std::atomic_bool& stopRequested) {
    started.set_value();
    co_await signal.wait();
    co_return !payload.empty() && stopRequested.load();
}

struct OwnedResult final {
    explicit OwnedResult(std::pmr::memory_resource* resource)
        : bytes(resource) {}
    std::pmr::string bytes;
};

ruvia::Task<OwnedResult> waitAndReturn(std::pmr::string payload, CountingResource& resultResource,
    ruvia::detail::WorkerSignal& signal, std::promise<void>& started) {
    started.set_value();
    co_await signal.wait();
    OwnedResult result(&resultResource);
    result.bytes.assign(2048, 'r');
    if (payload.empty()) {
        throw std::runtime_error("payload unexpectedly empty");
    }
    co_return result;
}

ruvia::Task<OwnedResult> returnWithPayload(std::pmr::string payload, CountingResource& resultResource) {
    OwnedResult result(&resultResource);
    result.bytes.assign(2048, 's');
    if (payload.empty()) {
        throw std::runtime_error("payload unexpectedly empty");
    }
    co_return result;
}

struct ThrowOnSecondMove final {
    ThrowOnSecondMove(CountingResource& resource, int& moves)
        : bytes(2048, 'm', &resource),
          moves(&moves) {}
    ThrowOnSecondMove(const ThrowOnSecondMove&) = delete;
    ThrowOnSecondMove& operator=(const ThrowOnSecondMove&) = delete;
    ThrowOnSecondMove(ThrowOnSecondMove&& other)
        : bytes(std::move(other.bytes)),
          moves(other.moves) {
        if (++*moves == 2) {
            throw std::runtime_error("result extraction failed");
        }
    }
    ThrowOnSecondMove& operator=(ThrowOnSecondMove&&) = delete;

    std::pmr::string bytes;
    int* moves;
};

ruvia::Task<ThrowOnSecondMove> waitAndReturnThrowingResult(std::pmr::string payload,
    CountingResource& resultResource, int& moves,
    ruvia::detail::WorkerSignal& signal, std::promise<void>& started) {
    started.set_value();
    co_await signal.wait();
    if (payload.empty()) {
        throw std::runtime_error("payload unexpectedly empty");
    }
    ThrowOnSecondMove result(resultResource, moves);
    co_return result;
}

ruvia::Task<void> awaitThrowingResult(ruvia::ScopedOperation<ThrowOnSecondMove>& operation) {
    static_cast<void>(co_await std::move(operation));
}

ruvia::Task<OwnedResult> awaitScopedResult(ruvia::ScopedOperation<OwnedResult>& operation) {
    co_return co_await std::move(operation);
}

ruvia::Task<bool> awaitScopedBool(ruvia::ScopedOperation<bool>& operation) {
    co_return co_await std::move(operation);
}

void recordStartCheck(void* target) noexcept {
    ++*static_cast<int*>(target);
}

ruvia::Task<void> completeImmediately() {
    co_return;
}

}  // namespace

RUVIA_TEST(scoped_operation_start_check_runs_before_beginning_task) {
    ruvia::detail::ScopedOperationScope scope;
    int checks = 0;
    auto operation = ruvia::detail::makeScopedOperation(
        scope, completeImmediately(), &recordStartCheck, &checks);
    auto root = ruvia::EventLoopPool({.loopCount = 1});
    const auto loop = root.loop(0);
    auto awaited = loop.start(awaitScopedOperation(operation));
    root.start();
    awaited.get();
    root.stop();
    root.join();
    RUVIA_CHECK_EQ(checks, 1);
}

RUVIA_TEST(scoped_operation_start_check_runs_before_cold_frame_destruction) {
    ruvia::detail::ScopedOperationScope scope;
    CountingResource resource;
    int checks = 0;
    {
        std::pmr::string payload(1024, 'p', &resource);
        auto operation = ruvia::detail::makeScopedOperation(
            scope, coldWithPayload(std::move(payload)), &recordStartCheck, &checks);
        RUVIA_CHECK_EQ(checks, 0);
    }
    RUVIA_CHECK_EQ(checks, 1);
    RUVIA_CHECK_EQ(resource.inUseAllocations, 0U);
    scope.close();
}

RUVIA_TEST(scoped_operation_close_and_join_releases_frame_before_expiring_capabilities) {
    ruvia::EventLoopPool loops({.loopCount = 1});
    const auto loop = loops.loop(0);
    const auto worker = loop.handle();
    ruvia::detail::WorkerSignal signal(worker);
    ruvia::detail::ScopedOperationScope scope;
    int expiredCount = 0;
    CountingResource parameterResource;
    std::size_t bytesAtExpire = static_cast<std::size_t>(-1);
    bool leaseActive = false;
    bool leaseAtExpire = true;
    TestScopedCapability capability(scope, expiredCount, &parameterResource, &bytesAtExpire,
        &leaseActive, &leaseAtExpire);
    static_cast<void>(capability);
    auto startedPromise = std::promise<void>();
    auto started = startedPromise.get_future();
    std::pmr::string payload(1024, 'p', &parameterResource);
    auto operation = ruvia::detail::makeScopedOperation(
        scope, waitWithPayload(std::move(payload), signal, startedPromise, FrameLease(leaseActive)));
    auto operationRoot = loop.start(awaitScopedOperation(operation));
    loops.start();
    started.get();

    auto joinRoot = loop.start(joinAndObserveBytes(scope, parameterResource));
    const auto notified = loop.post([&] { signal.notify(); });
    const auto bytesAtJoin = joinRoot.get();
    operationRoot.get();

    RUVIA_CHECK(notified == ruvia::PostStatus::kAccepted);
    RUVIA_CHECK_EQ(expiredCount, 1);
    RUVIA_CHECK_EQ(bytesAtExpire, 0U);
    RUVIA_CHECK_EQ(parameterResource.inUseAllocations, 0U);
    RUVIA_CHECK_EQ(bytesAtJoin, 0U);
    RUVIA_CHECK(!leaseAtExpire);
    RUVIA_CHECK(!leaseActive);
    loops.stop();
    loops.join();
}

RUVIA_TEST(scoped_operation_exception_releases_frame_before_expiring_capabilities) {
    ruvia::EventLoopPool loops({.loopCount = 1});
    const auto loop = loops.loop(0);
    const auto worker = loop.handle();
    ruvia::detail::WorkerSignal signal(worker);
    ruvia::detail::ScopedOperationScope scope;
    int expiredCount = 0;
    CountingResource resource;
    std::size_t bytesAtExpire = static_cast<std::size_t>(-1);
    TestScopedCapability capability(scope, expiredCount, &resource, &bytesAtExpire);
    static_cast<void>(capability);
    std::promise<void> startedPromise;
    auto started = startedPromise.get_future();
    std::pmr::string payload(1024, 'x', &resource);
    auto operation = ruvia::detail::makeScopedOperation(
        scope, waitThenThrow(std::move(payload), signal, startedPromise));
    auto operationRoot = loop.start(awaitScopedOperation(operation));
    loops.start();
    started.get();

    auto joinRoot = loop.start(joinAndObserveBytes(scope, resource));
    const auto notified = loop.post([&] { signal.notify(); });
    RUVIA_CHECK_EQ(joinRoot.get(), 0U);
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { operationRoot.get(); }));
    RUVIA_CHECK(notified == ruvia::PostStatus::kAccepted);
    RUVIA_CHECK_EQ(expiredCount, 1);
    RUVIA_CHECK_EQ(bytesAtExpire, 0U);
    RUVIA_CHECK_EQ(resource.inUseAllocations, 0U);
    loops.stop();
    loops.join();
}

RUVIA_TEST(scoped_operation_cooperative_cancellation_releases_frame_before_expiring_capabilities) {
    ruvia::EventLoopPool loops({.loopCount = 1});
    const auto loop = loops.loop(0);
    const auto worker = loop.handle();
    ruvia::detail::WorkerSignal signal(worker);
    ruvia::detail::ScopedOperationScope scope;
    int expiredCount = 0;
    CountingResource resource;
    std::size_t bytesAtExpire = static_cast<std::size_t>(-1);
    TestScopedCapability capability(scope, expiredCount, &resource, &bytesAtExpire);
    static_cast<void>(capability);
    std::promise<void> startedPromise;
    auto started = startedPromise.get_future();
    std::atomic_bool stopRequested{false};
    std::pmr::string payload(1024, 'x', &resource);
    auto operation = ruvia::detail::makeScopedOperation(scope,
        waitThenObserveCancellation(std::move(payload), signal, startedPromise, stopRequested));
    auto operationRoot = loop.start(awaitScopedBool(operation));
    loops.start();
    started.get();

    auto joinRoot = loop.start(joinAndObserveBytes(scope, resource));
    stopRequested.store(true);
    const auto notified = loop.post([&] { signal.notify(); });
    RUVIA_CHECK_EQ(joinRoot.get(), 0U);
    RUVIA_CHECK(operationRoot.get());
    RUVIA_CHECK(notified == ruvia::PostStatus::kAccepted);
    RUVIA_CHECK_EQ(expiredCount, 1);
    RUVIA_CHECK_EQ(bytesAtExpire, 0U);
    RUVIA_CHECK_EQ(resource.inUseAllocations, 0U);
    loops.stop();
    loops.join();
}

RUVIA_TEST(scoped_operation_result_survives_join_after_frame_release) {
    ruvia::EventLoopPool loops({.loopCount = 1});
    const auto loop = loops.loop(0);
    const auto worker = loop.handle();
    ruvia::detail::WorkerSignal signal(worker);
    ruvia::detail::ScopedOperationScope scope;
    int expiredCount = 0;
    CountingResource parameterResource;
    CountingResource resultResource;
    std::size_t bytesAtExpire = static_cast<std::size_t>(-1);
    TestScopedCapability capability(scope, expiredCount, &parameterResource, &bytesAtExpire);
    static_cast<void>(capability);
    std::promise<void> startedPromise;
    auto started = startedPromise.get_future();
    std::pmr::string payload(1024, 'p', &parameterResource);
    auto operation = ruvia::detail::makeScopedOperation(scope,
        waitAndReturn(std::move(payload), resultResource, signal, startedPromise));
    auto operationRoot = loop.start(awaitScopedResult(operation));
    loops.start();
    started.get();

    auto joinRoot = loop.start(joinAndObserveBytes(scope, parameterResource));
    const auto notified = loop.post([&] { signal.notify(); });
    RUVIA_CHECK_EQ(joinRoot.get(), 0U);
    {
        OwnedResult result = operationRoot.get();
        RUVIA_CHECK(notified == ruvia::PostStatus::kAccepted);
        RUVIA_CHECK_EQ(expiredCount, 1);
        RUVIA_CHECK_EQ(bytesAtExpire, 0U);
        RUVIA_CHECK_EQ(parameterResource.inUseAllocations, 0U);
        RUVIA_CHECK_EQ(result.bytes.size(), 2048U);
        RUVIA_CHECK_EQ(resultResource.inUseAllocations, 1U);
        {
            ruvia::detail::ScopedOperationScope repeatedScope;
            auto repeated = ruvia::detail::makeScopedOperation(repeatedScope,
                returnWithPayload(std::pmr::string(1024, 'q', &parameterResource), resultResource));
            auto repeatedRoot = loop.start(awaitScopedResult(repeated));
            auto repeatedResult = repeatedRoot.get();
            RUVIA_CHECK_EQ(parameterResource.inUseAllocations, 0U);
            RUVIA_CHECK_EQ(resultResource.inUseAllocations, 2U);
            RUVIA_CHECK_EQ(result.bytes.find_first_not_of('r'), std::pmr::string::npos);
            RUVIA_CHECK_EQ(repeatedResult.bytes.size(), 2048U);
            RUVIA_CHECK_EQ(repeatedResult.bytes.find_first_not_of('s'), std::pmr::string::npos);
        }
        RUVIA_CHECK_EQ(resultResource.inUseAllocations, 1U);
    }
    RUVIA_CHECK_EQ(resultResource.inUseAllocations, 0U);
    loops.stop();
    loops.join();
}

RUVIA_TEST(scoped_operation_result_move_failure_releases_frame_before_join) {
    ruvia::EventLoopPool loops({.loopCount = 1});
    const auto loop = loops.loop(0);
    const auto worker = loop.handle();
    ruvia::detail::WorkerSignal signal(worker);
    ruvia::detail::ScopedOperationScope scope;
    CountingResource parameterResource;
    CountingResource resultResource;
    int moves = 0;
    int expiredCount = 0;
    std::size_t bytesAtExpire = std::numeric_limits<std::size_t>::max();
    TestScopedCapability capability(scope, expiredCount, &parameterResource, &bytesAtExpire);
    std::promise<void> startedPromise;
    auto started = startedPromise.get_future();
    auto operation = ruvia::detail::makeScopedOperation(scope,
        waitAndReturnThrowingResult(std::pmr::string(1024, 'p', &parameterResource),
            resultResource, moves, signal, startedPromise));
    auto operationRoot = loop.start(awaitThrowingResult(operation));
    loops.start();
    started.get();

    auto joinRoot = loop.start(joinAndObserveBytes(scope, parameterResource));
    const auto notified = loop.post([&] { signal.notify(); });
    RUVIA_CHECK_EQ(joinRoot.get(), 0U);
    bool failedAtExtraction = false;
    try {
        operationRoot.get();
    } catch (const std::runtime_error& error) {
        failedAtExtraction = std::string_view(error.what()) == "result extraction failed";
    }
    RUVIA_CHECK(failedAtExtraction);
    RUVIA_CHECK(notified == ruvia::PostStatus::kAccepted);
    RUVIA_CHECK_EQ(moves, 2);
    RUVIA_CHECK_EQ(expiredCount, 1);
    RUVIA_CHECK_EQ(bytesAtExpire, 0U);
    RUVIA_CHECK_EQ(parameterResource.inUseAllocations, 0U);
    RUVIA_CHECK_EQ(resultResource.inUseAllocations, 0U);
    loops.stop();
    loops.join();
}

RUVIA_TEST(scoped_operation_scope_close_reclaims_cold_frame_before_capability_expiry) {
    ruvia::detail::ScopedOperationScope scope;
    CountingResource resource;
    int expiredCount = 0;
    std::size_t bytesAtExpire = std::numeric_limits<std::size_t>::max();
    TestScopedCapability capability(scope, expiredCount, &resource, &bytesAtExpire);
    auto operation = ruvia::detail::makeScopedOperation(
        scope, coldWithPayload(std::pmr::string(1024, 'p', &resource)));
    RUVIA_CHECK(resource.inUseAllocations != 0);
    scope.close();
    RUVIA_CHECK_EQ(expiredCount, 1);
    RUVIA_CHECK_EQ(bytesAtExpire, 0U);
    RUVIA_CHECK_EQ(resource.inUseAllocations, 0U);
    RUVIA_CHECK_EQ(resource.inUseBytes, 0U);
    RUVIA_CHECK(!scope.hasPendingOperations());
    // The expired public operation may remain alive after its frame is gone.
    static_cast<void>(operation);
}
