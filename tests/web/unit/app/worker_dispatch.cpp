#include <atomic>
#include <barrier>
#include <memory>
#include <semaphore>
#include <thread>
#include <utility>

#include <asio/io_context.hpp>

#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/web/WebWorker.h"
#include "ruvia/web/detail/app/WebWorkerDispatch.h"
#include "ruvia/web/detail/integration/WorkerCapabilities.h"

#include "test_harness.h"

namespace {

struct WorkerDispatchFixture final {
    asio::io_context ioContext;
    std::shared_ptr<ruvia::detail::WorkerDispatcher> dispatcher =
        std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 1);
    ruvia::WorkerHandle worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    ruvia::WorkerMemory memory;
    ruvia::detail::WorkerCapabilities capabilities{
        ioContext, worker, memory.resource(), {}, {}};
    std::shared_ptr<ruvia::detail::WebWorkerDispatch> dispatch;

    WorkerDispatchFixture()
        : dispatch(std::make_shared<ruvia::detail::WebWorkerDispatch>(ioContext.get_executor(),
              worker, memory.resource(), capabilities,
              [](std::exception_ptr) noexcept {})) {
        capabilities.initializeWorkerState();
    }

    void retire() {
        dispatcher->detachContext();
        dispatch->retire();
        capabilities.closeNow();
        capabilities.shutdownWorkerState();
    }
};

ruvia::Task<void> emptyTask(ruvia::WebWorkerContext&) {
    co_return;
}

}  // namespace

RUVIA_TEST(web_worker_dispatch_completes_started_task_and_releases_reservation) {
    WorkerDispatchFixture fixture;
    std::atomic_bool ran{false};

    const auto result = fixture.dispatch->handle().post([&ran](ruvia::WebWorkerContext& context) {
        ran.store(true, std::memory_order_release);
        return emptyTask(context);
    });
    RUVIA_CHECK(result.accepted());

    fixture.ioContext.run();

    const auto stats = fixture.dispatch->stats();
    RUVIA_CHECK(ran.load(std::memory_order_acquire));
    RUVIA_CHECK_EQ(stats.completed, 1U);
    RUVIA_CHECK_EQ(stats.outstanding, 0U);
    fixture.retire();
}

RUVIA_TEST(web_worker_dispatch_reconciles_rejected_and_abandoned_posts) {
    WorkerDispatchFixture fixture;

    const auto accepted = fixture.dispatch->handle().post(emptyTask);
    RUVIA_CHECK(accepted.accepted());
    auto full = fixture.dispatch->handle().post(emptyTask);
    RUVIA_CHECK_EQ(full.status(), ruvia::PostStatus::kQueueFull);
    RUVIA_CHECK(full.rejected() != nullptr);

    fixture.dispatch->close();

    auto stopped = fixture.dispatch->handle().post(emptyTask);
    RUVIA_CHECK_EQ(stopped.status(), ruvia::PostStatus::kWorkerStopping);
    RUVIA_CHECK(stopped.rejected() != nullptr);

    fixture.retire();
    RUVIA_CHECK_EQ(fixture.dispatch->stats().outstanding, 0U);
}

RUVIA_TEST(web_worker_dispatch_retires_late_factory_producer) {
    WorkerDispatchFixture fixture;
    struct State final {
        std::barrier<> producerReady{2};
        std::binary_semaphore factoryEntered{0};
        std::binary_semaphore releaseFactory{0};
        ruvia::detail::WebWorkerDispatch* dispatch{nullptr};
        std::atomic_bool paused{false};
        std::atomic_bool ran{false};
        std::atomic_uint32_t liveDestructions{0};
    } state;
    state.dispatch = fixture.dispatch.get();

    struct Producer final {
        State* state;
        bool live = true;

        explicit Producer(State& value) noexcept
            : state(&value) {}

        Producer(const Producer&) = delete;
        Producer& operator=(const Producer&) = delete;

        Producer(Producer&& other) noexcept
            : state(other.state),
              live(std::exchange(other.live, false)) {
            if (state->dispatch->stats().outstanding != 0 &&
                !state->paused.exchange(true, std::memory_order_acq_rel)) {
                state->factoryEntered.release();
                state->releaseFactory.acquire();
            }
        }

        ~Producer() {
            if (live) {
                state->liveDestructions.fetch_add(1, std::memory_order_relaxed);
            }
        }

        ruvia::Task<void> operator()(ruvia::WebWorkerContext&) {
            state->ran.store(true, std::memory_order_release);
            co_return;
        }
    };

    std::atomic<ruvia::PostStatus> status{ruvia::PostStatus::kWorkerStopping};
    std::thread producer([&] {
        state.producerReady.arrive_and_wait();
        const auto result = fixture.dispatch->handle().post(Producer{state});
        status.store(result.status(), std::memory_order_release);
    });
    state.producerReady.arrive_and_wait();
    state.factoryEntered.acquire();

    fixture.retire();
    state.releaseFactory.release();
    producer.join();

    const auto stats = fixture.dispatch->stats();
    RUVIA_CHECK_EQ(status.load(std::memory_order_acquire), ruvia::PostStatus::kAccepted);
    RUVIA_CHECK(!state.ran.load(std::memory_order_acquire));
    RUVIA_CHECK_EQ(state.liveDestructions.load(std::memory_order_acquire), 1U);
    RUVIA_CHECK_EQ(stats.completed, 0U);
    RUVIA_CHECK_EQ(stats.outstanding, 0U);
}
