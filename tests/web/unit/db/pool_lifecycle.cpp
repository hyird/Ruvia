#include <array>
#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <stdexcept>

#include "ruvia/core/EventLoopPool.h"

#include "db/DbPoolOperations.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

struct pool_backend final {
    struct config final {
        ruvia::DbDriver driver;
        std::chrono::milliseconds acquireTimeout{100};
    } config_;
    struct slot final {
        std::uint64_t cancellationId{};
        ruvia::detail::DbSlotAbortReason abortReason{ruvia::detail::DbSlotAbortReason::kNone};
        unsigned connects{};
        unsigned closes{};
    };

    pool_backend(const ruvia::WorkerHandle& worker, ruvia::DbDriver driver,
        std::pmr::memory_resource* resource)
        : config_{driver},
          scheduler_(2, worker, resource),
          lifecycle_(*this) {}

    ruvia::Task<void> connectUnlocked(slot& connection, const ruvia::OperationTimeout&) {
        ++connection.connects;
        if (fail_connect_ && &connection == &slots_.back()) {
            throw std::runtime_error("backend startup failed");
        }
        co_return;
    }

    void closeSlot(slot& connection) noexcept {
        ++connection.closes;
    }

    std::array<slot, 2> slots_;
    ruvia::PoolLeaseScheduler scheduler_;
    ruvia::detail::db_pool_lifecycle<pool_backend> lifecycle_;
    bool fail_connect_{};
};

ruvia::Task<void> exercise_pool(const ruvia::WorkerHandle& worker, ruvia::DbDriver driver,
    std::pmr::memory_resource* resource, ruvia::testing::TestContext& ruvia_ctx) {
    pool_backend backend(worker, driver, resource);
    auto& lifecycle = backend.lifecycle_;
    co_await lifecycle.connect();
    RUVIA_CHECK(backend.slots_[0].connects == 1 && backend.slots_[1].connects == 1);
    const auto first = co_await lifecycle.acquireSlot(ruvia::OperationTimeout(std::nullopt), {});
    const auto second = co_await lifecycle.acquireSlot(ruvia::OperationTimeout(std::nullopt), {});
    RUVIA_CHECK(first != second);

    ruvia::StopSource cancelled;
    cancelled.requestStop();
    bool cancellation_observed{};
    try {
        (void)co_await lifecycle.acquireSlot(ruvia::OperationTimeout(std::nullopt), cancelled.token());
    } catch (const ruvia::DbError& error) {
        cancellation_observed = error.code() == ruvia::DbError::Code::kCancelled;
    }
    RUVIA_CHECK(cancellation_observed);

    auto& active = backend.slots_[first];
    active.cancellationId = 42;
    lifecycle.cancelOperationById(43);
    RUVIA_CHECK(active.closes == 0);
    lifecycle.cancelOperationById(42);
    RUVIA_CHECK(active.closes == 1);
    bool operation_cancelled{};
    try {
        lifecycle.throwIfCancelled(active);
    } catch (const ruvia::DbError& error) {
        operation_cancelled = error.code() == ruvia::DbError::Code::kCancelled;
    }
    RUVIA_CHECK(operation_cancelled);

    lifecycle.closeNow();
    lifecycle.releaseSlot(first);
    lifecycle.releaseSlot(second);
    bool closing_observed{};
    try {
        (void)co_await lifecycle.acquireSlot(ruvia::OperationTimeout(std::nullopt), {});
    } catch (const ruvia::DbError& error) {
        closing_observed = error.code() == ruvia::DbError::Code::kClosing;
    }
    RUVIA_CHECK(closing_observed);
    RUVIA_CHECK(backend.slots_[first].closes == 2 && backend.slots_[second].closes == 1);

    pool_backend failing(worker, driver, resource);
    failing.fail_connect_ = true;
    bool startup_failed{};
    try {
        co_await failing.lifecycle_.connect();
    } catch (const std::runtime_error&) {
        startup_failed = true;
    }
    RUVIA_CHECK(startup_failed);
    failing.lifecycle_.closeNow();
    RUVIA_CHECK(failing.slots_[0].closes == 1 && failing.slots_[1].closes == 1);
}

}  // namespace

RUVIA_TEST(database_pool_lifecycle_schedules_cancels_and_closes_backend_slots) {
    ruvia::test::CountingMemoryResource memory;
    ruvia::EventLoopPool pool({.loopCount = 1});
    const auto worker = pool.loop(0).handle();
    pool.start();
    for (auto driver : {ruvia::DbDriver::kMariaDb, ruvia::DbDriver::kPostgreSql}) {
        pool.loop(0).start(exercise_pool(worker, driver, &memory, ruvia_ctx)).get();
    }
    pool.stop();
    pool.join();
    RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
}
