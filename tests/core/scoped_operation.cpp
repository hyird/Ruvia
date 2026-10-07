#include <atomic>
#include <future>
#include <limits>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <utility>

#include "ruvia/core/EventLoopPool.h"
#include "ruvia/core/ScopedOperation.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/WorkerSignal.h"

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

class TestScopedCapability final {
public:
    TestScopedCapability(ruvia::operation_scope& scope, int& expiredCount,
        CountingResource* resource = nullptr, std::size_t* bytesAtExpire = nullptr,
        const bool* leaseActive = nullptr, bool* leaseAtExpire = nullptr) noexcept
        : expiredCount_(&expiredCount),
          resource_(resource),
          bytesAtExpire_(bytesAtExpire),
          leaseActive_(leaseActive),
          leaseAtExpire_(leaseAtExpire),
          registration_(scope, this, &TestScopedCapability::expire) {}

    TestScopedCapability(const TestScopedCapability& other) noexcept
        : expiredCount_(other.expiredCount_),
          resource_(other.resource_),
          bytesAtExpire_(other.bytesAtExpire_),
          leaseActive_(other.leaseActive_),
          leaseAtExpire_(other.leaseAtExpire_),
          registration_(other.registration_, this) {}

    TestScopedCapability(TestScopedCapability&& other) noexcept
        : expiredCount_(std::exchange(other.expiredCount_, nullptr)),
          resource_(other.resource_),
          bytesAtExpire_(other.bytesAtExpire_),
          leaseActive_(other.leaseActive_),
          leaseAtExpire_(other.leaseAtExpire_),
          registration_(std::move(other.registration_), this) {}

    void use() const {
        registration_.require_active();
    }

private:
    static void expire(void* target) noexcept {
        auto& capability = *static_cast<TestScopedCapability*>(target);
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
    ruvia::scoped_capability_registration registration_;
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
    ruvia::operation_scope& scope, const CountingResource& resource) {
    co_await scope.close_and_join();
    co_return resource.inUseBytes;
}

ruvia::Task<void> waitWithPayload(std::pmr::string payload,
    ruvia::WorkerSignal& signal, std::promise<void>& started, FrameLease lease) {
    static_cast<void>(lease);
    started.set_value();
    co_await signal.wait();
    if (payload.empty()) {
        throw std::runtime_error("payload unexpectedly empty");
    }
}

ruvia::Task<void> waitThenThrow(std::pmr::string payload,
    ruvia::WorkerSignal& signal, std::promise<void>& started) {
    started.set_value();
    co_await signal.wait();
    if (!payload.empty()) {
        throw std::runtime_error("operation failure");
    }
}

ruvia::Task<bool> waitThenObserveCancellation(std::pmr::string payload,
    ruvia::WorkerSignal& signal, std::promise<void>& started,
    std::atomic_bool& stopRequested) {
    started.set_value();
    co_await signal.wait();
    co_return !payload.empty() && stopRequested.load();
}

// A moved-from result can still borrow its owner (as MSVC's PMR string debug
// proxy does). Track those borrows without relying on a library's string layout.
class delivery_borrow final {
public:
    explicit delivery_borrow(std::atomic_uint& borrows) noexcept
        : borrows_(&borrows) {
        borrows_->fetch_add(1);
    }
    delivery_borrow(const delivery_borrow&) = delete;
    delivery_borrow& operator=(const delivery_borrow&) = delete;
    delivery_borrow(delivery_borrow&& other) noexcept
        : delivery_borrow(*other.borrows_) {}
    delivery_borrow& operator=(delivery_borrow&&) = delete;
    ~delivery_borrow() {
        borrows_->fetch_sub(1);
    }

private:
    std::atomic_uint* borrows_;
};

ruvia::Task<delivery_borrow> return_delivery_borrow(std::atomic_uint& borrows, bool fail) {
    if (fail) {
        throw std::runtime_error("delivery failed");
    }
    co_return delivery_borrow(borrows);
}

ruvia::Task<delivery_borrow> return_cold_delivery_borrow(delivery_borrow borrow) {
    co_return std::move(borrow);
}

struct OwnedResult final {
    explicit OwnedResult(std::pmr::memory_resource* resource)
        : bytes(resource) {}
    std::pmr::string bytes;
};

ruvia::Task<OwnedResult> waitAndReturn(std::pmr::string payload, CountingResource& resultResource,
    ruvia::WorkerSignal& signal, std::promise<void>& started) {
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
    ruvia::WorkerSignal& signal, std::promise<void>& started) {
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

struct CheckerTarget final {
    bool alive{true};
    int calls{};
    int invalidCalls{};
    std::size_t allocatedBytesAtCheck{};
    CountingResource* resource{};
};

void checkTarget(void* target) noexcept {
    auto& checker = *static_cast<CheckerTarget*>(target);
    ++checker.calls;
    if (!checker.alive) {
        ++checker.invalidCalls;
    }
    if (checker.resource != nullptr) {
        checker.allocatedBytesAtCheck = checker.resource->inUseBytes;
    }
}

ruvia::Task<void> observeCheckAtTaskStart(CheckerTarget& checker, int& checksAtStart) {
    checksAtStart = checker.calls;
    co_return;
}

ruvia::Task<void> completeImmediately() {
    co_return;
}

struct ReentrantRetirementStats final {
    bool childSpawned{};
    bool pendingOperationsAtSpawn{};
    int expirationsAtSpawn{};
    std::size_t bytesAtSpawn{};
    std::size_t allocationsAfterJoin{};
    std::size_t bytesAfterJoin{};
    int expirationsAfterJoin{};
};

ruvia::Task<void> joinAndObserveTask(ruvia::operation_scope& scope,
    CountingResource& resource, int& expiredCount, ReentrantRetirementStats& stats) {
    co_await scope.close_and_join();
    stats.allocationsAfterJoin = resource.inUseAllocations;
    stats.bytesAfterJoin = resource.inUseBytes;
    stats.expirationsAfterJoin = expiredCount;
}

struct ReentrantFrameInput final {
    ReentrantFrameInput(std::pmr::memory_resource* resource, ruvia::TaskScope& children,
        ruvia::operation_scope& parentScope, CountingResource& countingResource,
        int& expiredCount, ReentrantRetirementStats& stats)
        : payload(1024, 'r', resource),
          children(&children),
          parentScope(&parentScope),
          countingResource(&countingResource),
          expiredCount(&expiredCount),
          stats(&stats) {}
    ReentrantFrameInput(const ReentrantFrameInput&) = delete;
    ReentrantFrameInput& operator=(const ReentrantFrameInput&) = delete;
    ReentrantFrameInput(ReentrantFrameInput&& other) noexcept
        : payload(std::move(other.payload)),
          children(std::exchange(other.children, nullptr)),
          parentScope(other.parentScope),
          countingResource(other.countingResource),
          expiredCount(other.expiredCount),
          stats(other.stats) {}
    ReentrantFrameInput& operator=(ReentrantFrameInput&&) = delete;
    ~ReentrantFrameInput() noexcept {
        if (children == nullptr) {
            return;
        }
        children->spawn(joinAndObserveTask(
            *parentScope, *countingResource, *expiredCount, *stats));
        stats->childSpawned = true;
        stats->pendingOperationsAtSpawn = parentScope->has_pending_operations();
        stats->expirationsAtSpawn = *expiredCount;
        stats->bytesAtSpawn = countingResource->inUseBytes;
    }

    std::pmr::string payload;
    ruvia::TaskScope* children;
    ruvia::operation_scope* parentScope;
    CountingResource* countingResource;
    int* expiredCount;
    ReentrantRetirementStats* stats;
};

ruvia::Task<void> coldWithReentrantInput(ReentrantFrameInput input) {
    if (input.payload.empty()) {
        throw std::runtime_error("payload unexpectedly empty");
    }
    co_return;
}

struct SiblingDropInput final {
    SiblingDropInput(CountingResource& resource,
        std::unique_ptr<ruvia::ScopedOperation<void>>& sibling)
        : payload(1024, 's', &resource),
          sibling(&sibling) {}
    SiblingDropInput(const SiblingDropInput&) = delete;
    SiblingDropInput& operator=(const SiblingDropInput&) = delete;
    SiblingDropInput(SiblingDropInput&& other) noexcept
        : payload(std::move(other.payload)),
          sibling(std::exchange(other.sibling, nullptr)) {}
    SiblingDropInput& operator=(SiblingDropInput&&) = delete;
    ~SiblingDropInput() {
        if (sibling != nullptr) {
            sibling->reset();
        }
    }

    std::pmr::string payload;
    std::unique_ptr<ruvia::ScopedOperation<void>>* sibling;
};

ruvia::Task<void> coldWithSiblingDrop(SiblingDropInput input) {
    if (input.payload.empty()) {
        throw std::runtime_error("payload unexpectedly empty");
    }
    co_return;
}

ruvia::Task<void> runReentrantColdDrop(const ruvia::WorkerHandle& worker,
    ruvia::operation_scope& parentScope, CountingResource& resource,
    int& expiredCount, ReentrantRetirementStats& stats, bool closeScope = false) {
    ruvia::TaskScope children(worker);
    {
        auto operation = ruvia::make_scoped_operation(
            parentScope, coldWithReentrantInput(ReentrantFrameInput(&resource, children, parentScope, resource, expiredCount, stats)));
        if (closeScope) {
            parentScope.close();
        }
        static_cast<void>(operation);
    }
    co_await children.join();
}

}  // namespace

RUVIA_TEST(scoped_operation_start_check_runs_before_task_body) {
    ruvia::operation_scope scope;
    CheckerTarget checker;
    int checksAtStart = 0;
    auto operation = ruvia::make_scoped_operation(
        scope, observeCheckAtTaskStart(checker, checksAtStart), &checkTarget, &checker);
    auto root = ruvia::EventLoopPool({.loopCount = 1});
    const auto loop = root.loop(0);
    auto awaited = loop.start(awaitScopedOperation(operation));
    root.start();
    awaited.get();
    root.stop();
    root.join();
    RUVIA_CHECK_EQ(checksAtStart, 1);
    RUVIA_CHECK_EQ(checker.calls, 2);
}

RUVIA_TEST(scoped_operation_start_check_runs_before_cold_frame_destruction) {
    ruvia::operation_scope scope;
    CountingResource resource;
    CheckerTarget checker{.resource = &resource};
    {
        std::pmr::string payload(1024, 'p', &resource);
        auto operation = ruvia::make_scoped_operation(
            scope, coldWithPayload(std::move(payload)), &checkTarget, &checker);
        RUVIA_CHECK_EQ(checker.calls, 0);
    }
    RUVIA_CHECK_EQ(checker.calls, 1);
    RUVIA_CHECK(checker.allocatedBytesAtCheck > 0U);
    RUVIA_CHECK_EQ(resource.inUseAllocations, 0U);
    RUVIA_CHECK_EQ(resource.inUseBytes, 0U);
    scope.close();
}

RUVIA_TEST(scoped_operation_close_and_join_releases_frame_before_expiring_capabilities) {
    ruvia::EventLoopPool loops({.loopCount = 1});
    const auto loop = loops.loop(0);
    const auto worker = loop.handle();
    ruvia::WorkerSignal signal(worker);
    ruvia::operation_scope scope;
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
    // Only the coroutine frame owns payload storage during the observation;
    // a moved-from local string can retain a debug iterator proxy.
    auto operation = ruvia::make_scoped_operation(
        scope, waitWithPayload(std::pmr::string(1024, 'p', &parameterResource), signal, startedPromise, FrameLease(leaseActive)));
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
    ruvia::WorkerSignal signal(worker);
    ruvia::operation_scope scope;
    int expiredCount = 0;
    CountingResource resource;
    std::size_t bytesAtExpire = static_cast<std::size_t>(-1);
    TestScopedCapability capability(scope, expiredCount, &resource, &bytesAtExpire);
    static_cast<void>(capability);
    std::promise<void> startedPromise;
    auto started = startedPromise.get_future();
    auto operation = ruvia::make_scoped_operation(
        scope, waitThenThrow(std::pmr::string(1024, 'x', &resource), signal, startedPromise));
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
    ruvia::WorkerSignal signal(worker);
    ruvia::operation_scope scope;
    int expiredCount = 0;
    CountingResource resource;
    std::size_t bytesAtExpire = static_cast<std::size_t>(-1);
    TestScopedCapability capability(scope, expiredCount, &resource, &bytesAtExpire);
    static_cast<void>(capability);
    std::promise<void> startedPromise;
    auto started = startedPromise.get_future();
    std::atomic_bool stopRequested{false};
    auto operation = ruvia::make_scoped_operation(scope,
        waitThenObserveCancellation(std::pmr::string(1024, 'x', &resource), signal, startedPromise, stopRequested));
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
    ruvia::WorkerSignal signal(worker);
    ruvia::operation_scope scope;
    int expiredCount = 0;
    CountingResource parameterResource;
    CountingResource resultResource;
    const auto allocations_per_result = [&] {
        const std::pmr::string sample(2048, 'r', &resultResource);
        return resultResource.inUseAllocations;
    }();
    std::size_t bytesAtExpire = static_cast<std::size_t>(-1);
    TestScopedCapability capability(scope, expiredCount, &parameterResource, &bytesAtExpire);
    static_cast<void>(capability);
    std::promise<void> startedPromise;
    auto started = startedPromise.get_future();
    auto operation = ruvia::make_scoped_operation(scope,
        waitAndReturn(std::pmr::string(1024, 'p', &parameterResource), resultResource, signal, startedPromise));
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
        RUVIA_CHECK_EQ(resultResource.inUseAllocations, allocations_per_result);
        {
            ruvia::operation_scope repeatedScope;
            auto repeated = ruvia::make_scoped_operation(repeatedScope,
                returnWithPayload(std::pmr::string(1024, 'q', &parameterResource), resultResource));
            auto repeatedRoot = loop.start(awaitScopedResult(repeated));
            auto repeatedResult = repeatedRoot.get();
            RUVIA_CHECK_EQ(parameterResource.inUseAllocations, 0U);
            RUVIA_CHECK_EQ(resultResource.inUseAllocations, 2 * allocations_per_result);
            RUVIA_CHECK_EQ(result.bytes.find_first_not_of('r'), std::pmr::string::npos);
            RUVIA_CHECK_EQ(repeatedResult.bytes.size(), 2048U);
            RUVIA_CHECK_EQ(repeatedResult.bytes.find_first_not_of('s'), std::pmr::string::npos);
        }
        RUVIA_CHECK_EQ(resultResource.inUseAllocations, allocations_per_result);
    }
    RUVIA_CHECK_EQ(resultResource.inUseAllocations, 0U);
    loops.stop();
    loops.join();
}

RUVIA_TEST(root_result_publication_retires_delivery_borrows_before_get_returns) {
    ruvia::EventLoopPool loops({.loopCount = 1});
    const auto loop = loops.loop(0);
    loops.start();
    std::atomic_uint borrows{0};
    for (unsigned repeat = 0; repeat != 64; ++repeat) {
        auto root = loop.start(return_delivery_borrow(borrows, false));
        {
            auto result = root.get();
            RUVIA_CHECK_EQ(borrows.load(), 1U);
        }
        RUVIA_CHECK_EQ(borrows.load(), 0U);
        auto failed = loop.start(return_delivery_borrow(borrows, true));
        RUVIA_CHECK(ruvia::testing::throwsOn([&] { (void)failed.get(); }));
        RUVIA_CHECK_EQ(borrows.load(), 0U);
    }
    loops.stop();
    loops.join();
}

RUVIA_TEST(root_rejected_launch_retires_cold_input_before_publishing_failure) {
    ruvia::EventLoopPool loops({.loopCount = 1});
    const auto loop = loops.loop(0);
    std::atomic_uint borrows{0};
    RUVIA_CHECK(loop.post([] { throw std::runtime_error("stop launch queue"); }) == ruvia::PostStatus::kAccepted);
    auto root = loop.start(return_cold_delivery_borrow(delivery_borrow(borrows)));
    RUVIA_CHECK_EQ(borrows.load(), 1U);
    loops.start();
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { (void)root.get(); }));
    RUVIA_CHECK_EQ(borrows.load(), 0U);
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { loops.join(); }));
}

RUVIA_TEST(scoped_operation_result_move_failure_releases_frame_before_join) {
    ruvia::EventLoopPool loops({.loopCount = 1});
    const auto loop = loops.loop(0);
    const auto worker = loop.handle();
    ruvia::WorkerSignal signal(worker);
    ruvia::operation_scope scope;
    CountingResource parameterResource;
    CountingResource resultResource;
    int moves = 0;
    int expiredCount = 0;
    std::size_t bytesAtExpire = std::numeric_limits<std::size_t>::max();
    TestScopedCapability capability(scope, expiredCount, &parameterResource, &bytesAtExpire);
    std::promise<void> startedPromise;
    auto started = startedPromise.get_future();
    auto operation = ruvia::make_scoped_operation(scope,
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
    ruvia::operation_scope scope;
    CountingResource resource;
    int expiredCount = 0;
    std::size_t bytesAtExpire = std::numeric_limits<std::size_t>::max();
    TestScopedCapability capability(scope, expiredCount, &resource, &bytesAtExpire);
    auto operation = ruvia::make_scoped_operation(
        scope, coldWithPayload(std::pmr::string(1024, 'p', &resource)));
    RUVIA_CHECK(resource.inUseAllocations != 0);
    scope.close();
    RUVIA_CHECK_EQ(expiredCount, 1);
    RUVIA_CHECK_EQ(bytesAtExpire, 0U);
    RUVIA_CHECK_EQ(resource.inUseAllocations, 0U);
    RUVIA_CHECK_EQ(resource.inUseBytes, 0U);
    RUVIA_CHECK(!scope.has_pending_operations());
    // The expired public operation may remain alive after its frame is gone.
    static_cast<void>(operation);
}

RUVIA_TEST(scoped_operation_expiration_clears_borrowed_start_check) {
    ruvia::operation_scope scope;
    CountingResource resource;
    CheckerTarget checker{.resource = &resource};
    {
        auto operation = ruvia::make_scoped_operation(scope,
            coldWithPayload(std::pmr::string(1024, 'p', &resource)), &checkTarget, &checker);
        RUVIA_CHECK(resource.inUseBytes > 0U);
        scope.close();
        RUVIA_CHECK_EQ(resource.inUseBytes, 0U);
        checker.alive = false;

        ruvia::EventLoopPool loops({.loopCount = 1});
        const auto loop = loops.loop(0);
        auto expiredRoot = loop.start(awaitScopedOperation(operation));
        loops.start();
        bool rejectedAsExpired = false;
        try {
            expiredRoot.get();
        } catch (const std::logic_error&) {
            rejectedAsExpired = true;
        }
        RUVIA_CHECK(rejectedAsExpired);
        loops.stop();
        loops.join();
    }
    RUVIA_CHECK_EQ(checker.calls, 1);
    RUVIA_CHECK_EQ(checker.invalidCalls, 0);
    RUVIA_CHECK_EQ(resource.inUseAllocations, 0U);
    RUVIA_CHECK_EQ(resource.inUseBytes, 0U);
}

RUVIA_TEST(scoped_operation_created_after_scope_close_discards_cold_frame) {
    ruvia::operation_scope scope;
    CountingResource resource;
    CheckerTarget checker{.resource = &resource};
    scope.close();
    checker.alive = false;

    auto operation = ruvia::make_scoped_operation(scope,
        coldWithPayload(std::pmr::string(1024, 'p', &resource)), &checkTarget, &checker);
    RUVIA_CHECK_EQ(resource.inUseAllocations, 0U);
    RUVIA_CHECK_EQ(resource.inUseBytes, 0U);

    ruvia::EventLoopPool loops({.loopCount = 1});
    const auto loop = loops.loop(0);
    auto expiredRoot = loop.start(awaitScopedOperation(operation));
    loops.start();
    bool rejectedAsExpired = false;
    try {
        expiredRoot.get();
    } catch (const std::logic_error&) {
        rejectedAsExpired = true;
    }
    RUVIA_CHECK(rejectedAsExpired);
    loops.stop();
    loops.join();

    RUVIA_CHECK_EQ(checker.calls, 0);
    RUVIA_CHECK_EQ(checker.invalidCalls, 0);
    RUVIA_CHECK_EQ(resource.inUseAllocations, 0U);
    RUVIA_CHECK_EQ(resource.inUseBytes, 0U);
}

RUVIA_TEST(scoped_operation_cold_frame_drop_allows_reentrant_parent_join) {
    ruvia::EventLoopPool loops({.loopCount = 1});
    const auto loop = loops.loop(0);
    const auto worker = loop.handle();
    ruvia::operation_scope parentScope;
    CountingResource resource;
    int expiredCount = 0;
    std::size_t bytesAtExpire = std::numeric_limits<std::size_t>::max();
    TestScopedCapability capability(parentScope, expiredCount, &resource, &bytesAtExpire);
    static_cast<void>(capability);
    ReentrantRetirementStats stats;

    auto root = loop.start(runReentrantColdDrop(
        worker, parentScope, resource, expiredCount, stats));
    loops.start();
    root.get();

    RUVIA_CHECK(stats.childSpawned);
    RUVIA_CHECK(stats.pendingOperationsAtSpawn);
    RUVIA_CHECK_EQ(stats.expirationsAtSpawn, 0);
    RUVIA_CHECK(stats.bytesAtSpawn > 0U);
    RUVIA_CHECK_EQ(expiredCount, 1);
    RUVIA_CHECK_EQ(bytesAtExpire, 0U);
    RUVIA_CHECK_EQ(stats.allocationsAfterJoin, 0U);
    RUVIA_CHECK_EQ(stats.bytesAfterJoin, 0U);
    RUVIA_CHECK_EQ(stats.expirationsAfterJoin, 1);
    RUVIA_CHECK_EQ(resource.inUseAllocations, 0U);
    RUVIA_CHECK_EQ(resource.inUseBytes, 0U);
    RUVIA_CHECK(!parentScope.has_pending_operations());
    loops.stop();
    loops.join();
}

RUVIA_TEST(scoped_operation_completion_clears_borrowed_start_check) {
    ruvia::operation_scope scope;
    CheckerTarget checker;
    {
        auto operation = ruvia::make_scoped_operation(
            scope, completeImmediately(), &checkTarget, &checker);
        ruvia::EventLoopPool loops({.loopCount = 1});
        const auto loop = loops.loop(0);
        auto firstRoot = loop.start(awaitScopedOperation(operation));
        loops.start();
        firstRoot.get();
        RUVIA_CHECK_EQ(checker.calls, 2);
        checker.alive = false;

        auto repeatedRoot = loop.start(awaitScopedOperation(operation));
        bool rejectedAsCompleted = false;
        try {
            repeatedRoot.get();
        } catch (const std::logic_error&) {
            rejectedAsCompleted = true;
        }
        RUVIA_CHECK(rejectedAsCompleted);
        loops.stop();
        loops.join();
    }
    RUVIA_CHECK_EQ(checker.calls, 2);
    RUVIA_CHECK_EQ(checker.invalidCalls, 0);
    scope.close();
}

RUVIA_TEST(scoped_operation_scope_drain_allows_reentrant_join_after_all_frames_release) {
    ruvia::EventLoopPool loops({.loopCount = 1});
    const auto loop = loops.loop(0);
    const auto worker = loop.handle();
    ruvia::operation_scope scope;
    CountingResource resource;
    int expiredCount = 0;
    std::size_t bytesAtExpire = std::numeric_limits<std::size_t>::max();
    TestScopedCapability capability(scope, expiredCount, &resource, &bytesAtExpire);
    ReentrantRetirementStats stats;
    auto root = loop.start(runReentrantColdDrop(
        worker, scope, resource, expiredCount, stats, true));
    loops.start();
    root.get();
    RUVIA_CHECK(stats.childSpawned);
    RUVIA_CHECK(stats.pendingOperationsAtSpawn);
    RUVIA_CHECK_EQ(stats.expirationsAtSpawn, 0);
    RUVIA_CHECK(stats.bytesAtSpawn > 0U);
    RUVIA_CHECK_EQ(expiredCount, 1);
    RUVIA_CHECK_EQ(bytesAtExpire, 0U);
    RUVIA_CHECK_EQ(stats.allocationsAfterJoin, 0U);
    RUVIA_CHECK_EQ(stats.bytesAfterJoin, 0U);
    RUVIA_CHECK_EQ(stats.expirationsAfterJoin, 1);
    RUVIA_CHECK_EQ(resource.inUseAllocations, 0U);
    RUVIA_CHECK(!scope.has_pending_operations());
    loops.stop();
    loops.join();
}

RUVIA_TEST(scoped_operation_join_rescans_after_frame_cleanup_drops_a_sibling) {
    ruvia::operation_scope scope;
    CountingResource resource;
    int expiredCount = 0;
    std::size_t bytesAtExpire = std::numeric_limits<std::size_t>::max();
    TestScopedCapability capability(scope, expiredCount, &resource, &bytesAtExpire);
    std::unique_ptr<ruvia::ScopedOperation<void>> sibling(
        new auto(ruvia::make_scoped_operation(scope,
            coldWithPayload(std::pmr::string(1024, 'p', &resource)))));
    const auto sibling_allocations = resource.inUseAllocations;
    auto operation = ruvia::make_scoped_operation(scope,
        coldWithSiblingDrop(SiblingDropInput(resource, sibling)));
    RUVIA_CHECK_EQ(resource.inUseAllocations, 2 * sibling_allocations);
    ruvia::EventLoopPool loops({.loopCount = 1});
    const auto loop = loops.loop(0);
    auto joined = loop.start(joinAndObserveBytes(scope, resource));
    loops.start();
    RUVIA_CHECK_EQ(joined.get(), 0U);
    RUVIA_CHECK(sibling == nullptr);
    RUVIA_CHECK_EQ(expiredCount, 1);
    RUVIA_CHECK_EQ(bytesAtExpire, 0U);
    RUVIA_CHECK_EQ(resource.inUseAllocations, 0U);
    RUVIA_CHECK_EQ(resource.inUseBytes, 0U);
    RUVIA_CHECK(!scope.has_pending_operations());
    loops.stop();
    loops.join();
    static_cast<void>(operation);
}

RUVIA_TEST(scoped_capabilities_copy_move_and_unregister_without_expiring_other_owners) {
    ruvia::operation_scope scope;
    int expired_count = 0;
    TestScopedCapability source(scope, expired_count);
    TestScopedCapability moved(std::move(source));
    {
        TestScopedCapability discarded(moved);
    }
    TestScopedCapability copied(moved);
    RUVIA_CHECK_EQ(expired_count, 0);
    scope.close();
    RUVIA_CHECK_EQ(expired_count, 2);

    TestScopedCapability expired_copy(copied);
    for (const auto* capability : {&source, &moved, &copied, &expired_copy}) {
        bool rejected = false;
        try {
            capability->use();
        } catch (const std::logic_error&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
    }
    scope.close();
    RUVIA_CHECK_EQ(expired_count, 2);
}

RUVIA_TEST(scoped_capability_cleanup_can_destroy_its_owner_and_a_sibling) {
    struct capability_owner final {
        capability_owner(ruvia::operation_scope& scope, CountingResource& resource,
            int& expired_count, bool& expired_before_cleanup,
            std::unique_ptr<capability_owner>* self,
            std::unique_ptr<capability_owner>* sibling)
            : payload(1024, 'p', &resource),
              expired_count(expired_count),
              expired_before_cleanup(expired_before_cleanup),
              self(self),
              sibling(sibling),
              registration(scope, this, &capability_owner::expire) {}

        static void expire(void* target) noexcept {
            auto& owner = *static_cast<capability_owner*>(target);
            ++owner.expired_count;
            try {
                owner.registration.require_active();
            } catch (const std::logic_error&) {
                owner.expired_before_cleanup = true;
            }
            auto* self = owner.self;
            auto* sibling = owner.sibling;
            if (sibling != nullptr) {
                sibling->reset();
            }
            if (self != nullptr) {
                self->reset();
            }
        }

        std::pmr::string payload;
        int& expired_count;
        bool& expired_before_cleanup;
        std::unique_ptr<capability_owner>* self;
        std::unique_ptr<capability_owner>* sibling;
        ruvia::scoped_capability_registration registration;
    };

    ruvia::operation_scope scope;
    CountingResource resource;
    int expired_count = 0;
    bool expired_before_cleanup = false;
    std::unique_ptr<capability_owner> self;
    auto sibling = std::make_unique<capability_owner>(
        scope, resource, expired_count, expired_before_cleanup, nullptr, nullptr);
    self = std::make_unique<capability_owner>(
        scope, resource, expired_count, expired_before_cleanup, &self, &sibling);
    scope.close();
    RUVIA_CHECK(self == nullptr);
    RUVIA_CHECK(sibling == nullptr);
    RUVIA_CHECK(expired_before_cleanup);
    RUVIA_CHECK_EQ(expired_count, 1);
    RUVIA_CHECK_EQ(resource.inUseAllocations, 0U);
    RUVIA_CHECK_EQ(resource.inUseBytes, 0U);
}
