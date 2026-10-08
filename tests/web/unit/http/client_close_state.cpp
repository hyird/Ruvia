#include <exception>
#include <memory>
#include <memory_resource>
#include <string>
#include <utility>

#include "ruvia/core/EventLoopPool.h"

#include "client/ClientCloseState.h"
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
    explicit CloseStateProbe(ruvia::EventLoop loop, bool* destroyed = nullptr)
        : loop_(std::move(loop)),
          worker_(loop_.handle()),
          state(std::make_unique<ruvia::detail::ClientCloseState>(loop_, worker_)),
          destroyed_(destroyed) {}

    ~CloseStateProbe() {
        state.reset();
        if (destroyed_ != nullptr) {
            *destroyed_ = true;
        }
    }

    ruvia::EventLoop loop_;
    ruvia::WorkerHandle worker_;
    std::unique_ptr<ruvia::detail::ClientCloseState> state;
    bool* destroyed_;
};

[[nodiscard]] std::exception_ptr makeFailure(std::pmr::memory_resource* resource) {
    try {
        throw PmrCloseFailure(resource);
    } catch (...) {
        return std::current_exception();
    }
}

ruvia::Task<void> cleanup(std::exception_ptr failure = {}) {
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
    co_return;
}

ruvia::Task<void> finishFailedClose(ruvia::EventLoop loop,
    std::pmr::memory_resource* resource, std::exception_ptr& original, bool& stateDestroyed,
    bool& callerObservedSameFailure, ruvia::testing::TestContext& ruvia_ctx) {
    const auto probe = std::make_shared<CloseStateProbe>(std::move(loop), &stateDestroyed);
    auto& state = *probe->state;
    original = makeFailure(resource);
    state.start_cleanup(probe, [failure = original] { return cleanup(failure); }, [probe](std::exception_ptr failure) { probe->state->finish(std::move(failure)); });
    // Retirement awaits the actual root task without re-reporting its failure.
    co_await state.shutdown_owned(probe, [] {}, ruvia::detail::ClientCloseState::ObservationMode::kRetirement);
    RUVIA_CHECK(state.complete());
    RUVIA_CHECK(state.taskStarted());

    callerObservedSameFailure = true;
    for (unsigned attempt = 0; attempt != 2; ++attempt) {
        bool observed = false;
        try {
            co_await state.shutdown_owned(probe, [] {}, ruvia::detail::ClientCloseState::ObservationMode::kCaller);
        } catch (const PmrCloseFailure& failure) {
            // rethrow_exception may copy its exception on some platforms.
            observed = failure.payload.get_allocator().resource() == resource &&
                       failure.payload.size() == 512 && failure.payload.front() == 'x';
        }
        callerObservedSameFailure = callerObservedSameFailure && observed;
    }
    bool restarted = false;
    state.start_cleanup(probe, [&restarted] {
        restarted = true;
        return cleanup(); }, [](std::exception_ptr) { std::terminate(); });
    RUVIA_CHECK(!restarted);
}

ruvia::Task<void> finishSuccessfulClose(ruvia::EventLoop loop, bool& coldStayedCold,
    bool& closeRemainedOneShot) {
    {
        const auto probe = std::make_shared<CloseStateProbe>(loop);
        auto& state = *probe->state;
        state.completeBeforePublication();
        coldStayedCold = true;
        state.start_cleanup(probe, [&coldStayedCold] {
            coldStayedCold = false;
            return cleanup(); }, [](std::exception_ptr) { std::terminate(); });
        state.observeFailure(ruvia::detail::ClientCloseState::ObservationMode::kCaller);
        state.observeFailure(ruvia::detail::ClientCloseState::ObservationMode::kRetirement);
    }
    {
        const auto probe = std::make_shared<CloseStateProbe>(loop);
        auto& state = *probe->state;
        state.start_cleanup(probe, [] { return cleanup(); }, [probe](std::exception_ptr failure) { probe->state->finish(std::move(failure)); });
        co_await state.shutdown_owned(probe, [] {}, ruvia::detail::ClientCloseState::ObservationMode::kCaller);
        closeRemainedOneShot = state.taskStarted() && state.complete();
        state.start_cleanup(probe, [&closeRemainedOneShot] {
            closeRemainedOneShot = false;
            return cleanup(); }, [](std::exception_ptr) { std::terminate(); });
    }
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
        RUVIA_CHECK(callerObservedSameFailure);
        RUVIA_CHECK(memory.liveAllocations() > 0);

        bool poolObservedFailure = false;
        try {
            pool.join();
        } catch (const PmrCloseFailure&) {
            poolObservedFailure = true;
        }
        RUVIA_CHECK(poolObservedFailure);
        RUVIA_CHECK(stateDestroyed);
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
