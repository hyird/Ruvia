#include "ruvia/core/worker_runtime.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <asio/steady_timer.hpp>

#include "ruvia/core/EventLoopPool.h"
#include "ruvia/core/WorkerSignal.h"

#include "test_harness.h"

namespace {

using state = ruvia::RuntimeLifecycle::State;

RUVIA_TEST(worker_runtime_drains_accepted_work_before_start) {
    ruvia::worker_runtime runtime;
    const auto worker = runtime.context().handle();
    const auto caller = std::this_thread::get_id();
    bool owner_affine{};
    bool ran{};
    RUVIA_CHECK(worker.post([&] {
                          owner_affine = worker.isCurrent() && std::this_thread::get_id() != caller;
                          ran = true;
                      })
            .accepted());
    runtime.join();
    RUVIA_CHECK(ran);
    RUVIA_CHECK(owner_affine);
    RUVIA_CHECK_EQ(runtime.state(), state::kStopped);
    RUVIA_CHECK(!worker.valid());
    RUVIA_CHECK(!worker.post([] {}).accepted());
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { runtime.start(); }));
}

RUVIA_TEST(worker_runtime_keeps_owner_until_external_producers_quiesce) {
    ruvia::worker_runtime runtime({.io_policy = ruvia::worker_io_policy::single_owner});
    const auto worker = runtime.context().handle();
    std::promise<void> ready;
    std::promise<void> admission_stopped;
    std::atomic<bool> stopped_on_owner{};
    bool finalized_on_owner{};
    bool shutdown_on_owner{};
    std::vector<int> phases;
    runtime.configure({
        .startup = [&] {
            phases.push_back(1);
            ready.set_value(); },
        .stop_admission = [&] {
            phases.push_back(2);
            stopped_on_owner.store(worker.isCurrent());
            admission_stopped.set_value(); },
        .shutdown = [&] {
            phases.push_back(4);
            shutdown_on_owner = worker.isCurrent(); },
    });
    runtime.start();
    ready.get_future().get();
    runtime.request_stop();
    admission_stopped.get_future().get();
    RUVIA_CHECK(stopped_on_owner.load());
    RUVIA_CHECK_EQ(runtime.state(), state::kStopping);
    RUVIA_CHECK(worker.valid());
    RUVIA_CHECK(!worker.accepting());
    RUVIA_CHECK(!runtime.post_control([] {}));
    runtime.finalize([&] {
        phases.push_back(3);
        finalized_on_owner = worker.isCurrent();
    });
    runtime.join();
    RUVIA_CHECK(finalized_on_owner);
    RUVIA_CHECK(shutdown_on_owner);
    RUVIA_CHECK_EQ(phases, (std::vector<int>{1, 2, 3, 4}));
    RUVIA_CHECK(!worker.valid());
}

RUVIA_TEST(worker_runtime_failure_cancels_then_drains_io_before_shutdown) {
    ruvia::worker_runtime runtime;
    const auto worker = runtime.context().handle();
    asio::steady_timer timer(runtime.context().ioContext());
    bool failure_on_owner{};
    bool completion_on_owner{};
    bool shutdown_after_completion{};
    std::exception_ptr reported;
    std::vector<int> phases;
    runtime.configure({
        .startup = [&] {
            timer.expires_after(std::chrono::hours(1));
            timer.async_wait([&](const asio::error_code& error) {
                completion_on_owner = worker.isCurrent() && error == asio::error::operation_aborted;
                phases.push_back(3);
            });
            throw std::runtime_error("owner initialization failed"); },
        .stop_admission = [&] {
            phases.push_back(2);
            timer.cancel();
            runtime.finalize(); },
        .failure = [&](std::exception_ptr error) noexcept {
            phases.push_back(1);
            failure_on_owner = worker.isCurrent();
            reported = error; },
        .shutdown = [&] {
            shutdown_after_completion = completion_on_owner && worker.isCurrent();
            phases.push_back(4); },
    });
    runtime.start();
    runtime.join();
    RUVIA_CHECK(failure_on_owner);
    RUVIA_CHECK(shutdown_after_completion);
    RUVIA_CHECK_EQ(reported, runtime.failure());
    RUVIA_CHECK_EQ(phases, (std::vector<int>{1, 2, 3, 4}));
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { runtime.rethrow_failure(); }));
}

RUVIA_TEST(worker_runtime_concurrent_stop_and_join_share_one_barrier) {
    for (int iteration = 0; iteration != 32; ++iteration) {
        ruvia::worker_runtime runtime;
        std::atomic<int> stops{};
        std::atomic<int> shutdowns{};
        runtime.configure({
            .stop_admission = [&] {
                ++stops;
                runtime.finalize(); },
            .shutdown = [&] { ++shutdowns; },
        });
        runtime.start();
        std::thread first([&] {
            runtime.request_stop();
            runtime.join();
        });
        std::thread second([&] {
            runtime.request_stop();
            runtime.join();
        });
        first.join();
        second.join();
        RUVIA_CHECK_EQ(stops.load(), 1);
        RUVIA_CHECK_EQ(shutdowns.load(), 1);
        RUVIA_CHECK_EQ(runtime.state(), state::kStopped);
    }
}

RUVIA_TEST(worker_runtime_finalize_before_start_retires_on_owner) {
    ruvia::worker_runtime runtime;
    const auto worker = runtime.context().handle();
    bool startup_ran{};
    bool stop_ran{};
    bool finalizer_ran{};
    bool shutdown_ran{};
    runtime.configure({
        .startup = [&] { startup_ran = true; },
        .stop_admission = [&] { stop_ran = worker.isCurrent(); },
        .shutdown = [&] { shutdown_ran = worker.isCurrent(); },
    });
    runtime.finalize([&] { finalizer_ran = worker.isCurrent(); });
    runtime.join();
    RUVIA_CHECK(!startup_ran);
    RUVIA_CHECK(stop_ran);
    RUVIA_CHECK(finalizer_ran);
    RUVIA_CHECK(shutdown_ran);
}

RUVIA_TEST(worker_runtime_rejects_self_join_without_losing_cleanup) {
    ruvia::worker_runtime runtime;
    bool rejected{};
    runtime.configure({
        .startup = [&] { rejected = ruvia::testing::throwsOn([&] { runtime.join(); }); },
    });
    runtime.start();
    runtime.join();
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(runtime.state(), state::kStopped);
}

ruvia::Task<void> hold_retirement(ruvia::WorkerSignal& gate, std::promise<void>& ready) {
    ready.set_value();
    co_await gate.wait();
}

struct cleanup_payload final {
    cleanup_payload(const ruvia::WorkerHandle& worker, bool& destroyed)
        : worker_(worker),
          destroyed_(destroyed) {}
    ~cleanup_payload() {
        destroyed_ = worker_.isCurrent();
    }
    const ruvia::WorkerHandle& worker_;
    bool& destroyed_;
};

RUVIA_TEST(event_loop_cleanup_drains_owned_inputs_after_business_admission_closes) {
    ruvia::EventLoopPool pool({.loopCount = 1});
    const auto loop = pool.loop(0);
    const auto worker = loop.handle();
    ruvia::WorkerSignal gate(worker);
    std::promise<void> retirement_started;
    auto stop_callback = loop.onStop([&] { return hold_retirement(gate, retirement_started); });
    pool.start();
    pool.stop();
    retirement_started.get_future().get();
    bool ran{};
    bool inputs_destroyed_on_owner{};
    RUVIA_CHECK(!loop.accepting());
    RUVIA_CHECK(loop.defer_cleanup([payload = std::make_unique<cleanup_payload>(worker, inputs_destroyed_on_owner), &gate, &ran, &worker] {
        ran = worker.isCurrent();
        gate.notify();
    }));
    pool.join();
    RUVIA_CHECK(ran);
    RUVIA_CHECK(inputs_destroyed_on_owner);
    RUVIA_CHECK(!loop.defer_cleanup([] {}));
}

RUVIA_TEST(event_loop_cleanup_can_be_drained_without_starting_business_workers) {
    ruvia::EventLoopPool pool({.loopCount = 1});
    const auto loop = pool.loop(0);
    const auto worker = loop.handle();
    bool ran{};
    RUVIA_CHECK(loop.defer_cleanup([&] { ran = worker.isCurrent(); }));
    pool.join();
    RUVIA_CHECK(ran);
    RUVIA_CHECK(!loop.valid());
}

RUVIA_TEST(worker_runtime_retains_control_payload_until_owner_completion) {
    ruvia::worker_runtime runtime;
    std::promise<void> initialized;
    std::promise<void> release_owner;
    auto release = release_owner.get_future();
    runtime.configure({.startup = [&] {
        initialized.set_value();
        release.wait();
    }});
    runtime.start();
    initialized.get_future().get();
    auto payload = std::make_shared<int>(7);
    std::weak_ptr<int> lifetime = payload;
    bool consumed{};
    RUVIA_CHECK(runtime.post_control([owned = std::move(payload), &consumed] { consumed = *owned == 7; }));
    RUVIA_CHECK(!lifetime.expired());
    runtime.request_stop();
    release_owner.set_value();
    runtime.join();
    RUVIA_CHECK(consumed);
    RUVIA_CHECK(lifetime.expired());
}

}  // namespace
