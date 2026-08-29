#include "test_harness.h"

#include <future>
#include <utility>

#include "ruvia/core/EventLoopPool.h"
#include "ruvia/core/ScopedOperation.h"
#include "ruvia/core/detail/worker/WorkerSignal.h"

namespace {

class TestScopedCapability final : private ruvia::detail::ScopedCapabilityNode {
public:
    TestScopedCapability(ruvia::detail::ScopedOperationScope& scope, int& expiredCount) noexcept
        : ScopedCapabilityNode(scope, &TestScopedCapability::expire),
          expiredCount_(&expiredCount) {}

private:
    static void expire(ruvia::detail::ScopedCapabilityNode& node) noexcept {
        auto& capability = static_cast<TestScopedCapability&>(node);
        ++*capability.expiredCount_;
    }

    int* expiredCount_;
};

ruvia::Task<void> waitForScopeRelease(ruvia::detail::WorkerSignal& signal, std::promise<void>& started) {
    started.set_value();
    co_await signal.wait();
}

ruvia::Task<void> awaitScopedOperation(ruvia::ScopedOperation<void>& operation) {
    co_await std::move(operation);
}

}  // namespace

RUVIA_TEST(scoped_operation_close_and_join_waits_before_expiring_capabilities) {
    ruvia::EventLoopPool loops({.loopCount = 1});
    const auto loop = loops.loop(0);
    const auto worker = loop.handle();
    ruvia::detail::WorkerSignal signal(worker);
    ruvia::detail::ScopedOperationScope scope;
    int expiredCount = 0;
    TestScopedCapability capability(scope, expiredCount);
    static_cast<void>(capability);
    auto startedPromise = std::promise<void>();
    auto started = startedPromise.get_future();
    auto operation = ruvia::detail::makeScopedOperation(scope, waitForScopeRelease(signal, startedPromise));
    auto operationRoot = loop.start(awaitScopedOperation(operation));
    loops.start();
    started.get();

    auto joinRoot = loop.start(scope.closeAndJoin());
    const auto notified = loop.post([&] { signal.notify(); });
    joinRoot.get();
    operationRoot.get();

    RUVIA_CHECK(notified == ruvia::PostStatus::kAccepted);
    RUVIA_CHECK_EQ(expiredCount, 1);
    loops.stop();
    loops.join();
}
