#include <exception>
#include <memory>
#include <memory_resource>
#include <string>
#include <utility>

#include "ruvia/core/EventLoopPool.h"
#include "ruvia/web/detail/client/ClientCloseState.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

struct PmrCloseFailure final {
    explicit PmrCloseFailure(std::pmr::memory_resource* resource)
        : payload(512, 'x', resource) {}

    PmrCloseFailure(const PmrCloseFailure& other)
        : payload(other.payload, other.payload.get_allocator().resource()) {}
    PmrCloseFailure(PmrCloseFailure&&) noexcept = default;

    std::pmr::string payload;
};

struct CloseStateProbe final {
    CloseStateProbe(ruvia::EventLoop loop, bool& destroyed)
        : loop_(std::move(loop)),
          worker_(loop_.handle()),
          state(std::make_unique<ruvia::detail::ClientCloseState>(loop_, worker_)),
          destroyed_(destroyed) {}

    ~CloseStateProbe() {
        state.reset();
        destroyed_ = true;
    }

    ruvia::EventLoop loop_;
    ruvia::WorkerHandle worker_;
    std::unique_ptr<ruvia::detail::ClientCloseState> state;
    bool& destroyed_;
};

[[nodiscard]] std::exception_ptr makeFailure(std::pmr::memory_resource* resource) {
    try {
        throw PmrCloseFailure(resource);
    } catch (...) {
        return std::current_exception();
    }
}

ruvia::Task<void> finishFailedClose(ruvia::EventLoop loop,
    std::pmr::memory_resource* resource, std::exception_ptr& original, bool& stateDestroyed,
    bool& callerObservedSameFailure, ruvia::testing::TestContext& ruvia_ctx) {
    CloseStateProbe probe(std::move(loop), stateDestroyed);
    auto& state = *probe.state;
    RUVIA_CHECK(state.startTask());
    original = makeFailure(resource);
    state.finish(original);
    RUVIA_CHECK(state.complete());

    // Retirement must confirm completion without throwing the already-reported
    // teardown failure back through the stop callback.
    state.observeFailure(ruvia::detail::ClientCloseState::ObservationMode::kRetirement);
    callerObservedSameFailure = true;
    for (unsigned attempt = 0; attempt != 2; ++attempt) {
        bool observed = false;
        try {
            state.observeFailure(ruvia::detail::ClientCloseState::ObservationMode::kCaller);
        } catch (const PmrCloseFailure& failure) {
            // rethrow_exception may copy its exception on some platforms.
            // Preserve the failure's payload and allocator, not object identity.
            observed = failure.payload.get_allocator().resource() == resource &&
                       failure.payload.size() == 512 && failure.payload.front() == 'x';
        }
        callerObservedSameFailure = callerObservedSameFailure && observed;
    }
    RUVIA_CHECK(!state.startTask());
    co_return;
}

ruvia::Task<void> finishSuccessfulClose(ruvia::EventLoop loop, bool& coldStayedCold,
    bool& closeRemainedOneShot) {
    const auto worker = loop.handle();
    {
        ruvia::detail::ClientCloseState cold(loop, worker);
        cold.completeBeforePublication();
        coldStayedCold = !cold.startTask();
        cold.observeFailure(ruvia::detail::ClientCloseState::ObservationMode::kCaller);
        cold.observeFailure(ruvia::detail::ClientCloseState::ObservationMode::kRetirement);
    }
    {
        ruvia::detail::ClientCloseState completed(loop, worker);
        closeRemainedOneShot = completed.startTask();
        completed.finish({});
        completed.observeFailure(ruvia::detail::ClientCloseState::ObservationMode::kCaller);
        completed.observeFailure(ruvia::detail::ClientCloseState::ObservationMode::kRetirement);
        closeRemainedOneShot = closeRemainedOneShot && !completed.startTask();
    }
    co_return;
}

}  // namespace

RUVIA_TEST(client_close_state_reports_unobserved_failure_and_rethrows_to_callers) {
    ruvia::test::CountingMemoryResource memory;
    std::exception_ptr original;
    bool stateDestroyed = false;
    bool callerObservedSameFailure = false;
    {
        ruvia::EventLoopPool pool({.loopCount = 1});
        pool.start();
        auto loop = pool.loop(0);
        loop.start(finishFailedClose(
                       loop, &memory, original, stateDestroyed, callerObservedSameFailure, ruvia_ctx))
            .get();
        RUVIA_CHECK(stateDestroyed);
        RUVIA_CHECK(callerObservedSameFailure);
        RUVIA_CHECK(memory.liveAllocations() > 0);

        bool poolObservedFailure = false;
        try {
            pool.join();
        } catch (const PmrCloseFailure&) {
            poolObservedFailure = true;
        }
        RUVIA_CHECK(poolObservedFailure);
    }
    original = {};
    RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
    RUVIA_CHECK(memory.deallocationCount() > 0);
}

RUVIA_TEST(client_close_state_successful_and_cold_completion_is_one_shot) {
    ruvia::EventLoopPool pool({.loopCount = 1});
    pool.start();
    bool coldStayedCold = false;
    bool closeRemainedOneShot = false;
    pool.loop(0).start(finishSuccessfulClose(
                           pool.loop(0), coldStayedCold, closeRemainedOneShot))
        .get();
    RUVIA_CHECK(coldStayedCold);
    RUVIA_CHECK(closeRemainedOneShot);
    pool.stop();
    pool.join();
}
