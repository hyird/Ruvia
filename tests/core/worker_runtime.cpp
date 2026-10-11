#include "ruvia/core/worker_runtime.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <asio/steady_timer.hpp>

#include "ruvia/core/event_loop_pool.h"
#include "ruvia/core/worker_group.h"
#include "ruvia/core/worker_signal.h"

#include "test_harness.h"

namespace {

using state = ruvia::runtime_lifecycle::state_type;

RUVIA_TEST(worker_runtime_drains_accepted_work_before_start) {
    ruvia::worker_runtime runtime;
    const auto worker_value = runtime.context().handle();
    const auto caller = std::this_thread::get_id();
    bool owner_affine{};
    bool ran{};
    RUVIA_CHECK(worker_value.post([&] {
                                owner_affine = worker_value.is_current() && std::this_thread::get_id() != caller;
                                ran = true;
                            })
            .accepted());
    runtime.join();
    RUVIA_CHECK(ran);
    RUVIA_CHECK(owner_affine);
    RUVIA_CHECK_EQ(runtime.state(), state::stopped);
    RUVIA_CHECK(!worker_value.valid());
    RUVIA_CHECK(!worker_value.post([] {}).accepted());
    RUVIA_CHECK(ruvia::testing::throws_on([&] { runtime.start(); }));
}

RUVIA_TEST(worker_runtime_keeps_owner_until_external_producers_quiesce) {
    ruvia::worker_runtime runtime({.io_policy_ = ruvia::worker_io_policy::single_owner});
    const auto worker_value = runtime.context().handle();
    std::promise<void> ready;
    std::promise<void> admission_stopped;
    std::atomic<bool> stopped_on_owner{};
    bool finalized_on_owner{};
    bool shutdown_on_owner{};
    std::vector<int> phases;
    runtime.configure({
        .startup_ = [&] {
            phases.push_back(1);
            ready.set_value(); },
        .stop_admission_ = [&] {
            phases.push_back(2);
            stopped_on_owner.store(worker_value.is_current());
            admission_stopped.set_value(); },
        .finalize_ = [&] {
            phases.push_back(3);
            finalized_on_owner = worker_value.is_current(); },
        .shutdown_ = [&] {
            phases.push_back(4);
            shutdown_on_owner = worker_value.is_current(); },
    });
    runtime.start();
    ready.get_future().get();
    runtime.request_stop();
    admission_stopped.get_future().get();
    RUVIA_CHECK(stopped_on_owner.load());
    RUVIA_CHECK_EQ(runtime.state(), state::stopping);
    RUVIA_CHECK(worker_value.valid());
    RUVIA_CHECK(!worker_value.accepting());
    RUVIA_CHECK(!runtime.post_control([] {}));
    runtime.finalize();
    runtime.join();
    RUVIA_CHECK(finalized_on_owner);
    RUVIA_CHECK(shutdown_on_owner);
    RUVIA_CHECK_EQ(phases, (std::vector<int>{1, 2, 3, 4}));
    RUVIA_CHECK(!worker_value.valid());
}

RUVIA_TEST(worker_runtime_failure_cancels_then_drains_io_before_shutdown) {
    ruvia::worker_runtime runtime;
    const auto worker_value = runtime.context().handle();
    asio::steady_timer timer(runtime.context().io_context());
    bool failure_on_owner{};
    bool completion_on_owner{};
    bool shutdown_after_completion{};
    std::exception_ptr reported;
    std::vector<int> phases;
    runtime.configure({
        .startup_ = [&] {
            timer.expires_after(std::chrono::hours(1));
            timer.async_wait([&](const asio::error_code& error) {
                completion_on_owner = worker_value.is_current() && error == asio::error::operation_aborted;
                phases.push_back(3);
            });
            throw std::runtime_error("owner initialization failed"); },
        .stop_admission_ = [&] {
            phases.push_back(2);
            timer.cancel();
            runtime.finalize(); },
        .failure_ = [&](std::exception_ptr error) noexcept {
            phases.push_back(1);
            failure_on_owner = worker_value.is_current();
            reported = error; },
        .shutdown_ = [&] {
            shutdown_after_completion = completion_on_owner && worker_value.is_current();
            phases.push_back(4); },
    });
    runtime.start();
    runtime.join();
    RUVIA_CHECK(failure_on_owner);
    RUVIA_CHECK(shutdown_after_completion);
    RUVIA_CHECK_EQ(reported, runtime.failure());
    RUVIA_CHECK_EQ(phases, (std::vector<int>{1, 2, 3, 4}));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { runtime.rethrow_failure(); }));
}

RUVIA_TEST(worker_runtime_concurrent_stop_and_join_share_one_barrier) {
    for (int iteration = 0; iteration != 32; ++iteration) {
        ruvia::worker_runtime runtime;
        std::atomic<int> stops{};
        std::atomic<int> shutdowns{};
        runtime.configure({
            .stop_admission_ = [&] {
                ++stops;
                runtime.finalize(); },
            .shutdown_ = [&] { ++shutdowns; },
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
        RUVIA_CHECK_EQ(runtime.state(), state::stopped);
    }
}

RUVIA_TEST(worker_runtime_finalize_before_start_retires_on_owner) {
    ruvia::worker_runtime runtime;
    const auto worker_value = runtime.context().handle();
    bool startup_ran{};
    bool stop_ran{};
    bool finalizer_ran{};
    bool shutdown_ran{};
    runtime.configure({
        .startup_ = [&] { startup_ran = true; },
        .stop_admission_ = [&] { stop_ran = worker_value.is_current(); },
        .finalize_ = [&] { finalizer_ran = worker_value.is_current(); },
        .shutdown_ = [&] { shutdown_ran = worker_value.is_current(); },
    });
    runtime.finalize();
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
        .startup_ = [&] { rejected = ruvia::testing::throws_on([&] { runtime.join(); }); },
    });
    runtime.start();
    runtime.join();
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(runtime.state(), state::stopped);
}

RUVIA_TEST(worker_runtime_serves_only_after_owner_published_readiness) {
    ruvia::worker_runtime runtime;
    const auto worker_value = runtime.context().handle();
    std::atomic<bool> served_on_owner{};
    std::atomic<int> serves{};
    runtime.configure({
        .startup_ = [&] {
            // Readiness may follow asynchronous startup work on the owner.
            RUVIA_CHECK(worker_value.post([&] { runtime.mark_ready(); }).accepted()); },
        .serve_ = [&] {
            ++serves;
            served_on_owner.store(worker_value.is_current()); },
    });
    runtime.start();
    RUVIA_CHECK(runtime.wait_until_ready());
    RUVIA_CHECK_EQ(serves.load(), 0);
    runtime.request_serve();
    runtime.request_serve();
    RUVIA_CHECK(runtime.wait_until_serving());
    RUVIA_CHECK_EQ(serves.load(), 1);
    RUVIA_CHECK(served_on_owner.load());
    runtime.join();
    RUVIA_CHECK(runtime.failure() == nullptr);
}

RUVIA_TEST(worker_runtime_stop_before_readiness_aborts_startup) {
    ruvia::worker_runtime runtime;
    std::atomic<bool> abandoned{};
    bool served{};
    runtime.configure({
        .serve_ = [&] { served = true; },
        .abandon_ = [&] { abandoned.store(true); },
    });
    runtime.start();
    runtime.request_stop();
    RUVIA_CHECK(!runtime.wait_until_ready());
    RUVIA_CHECK(!runtime.wait_until_serving());
    runtime.join();
    RUVIA_CHECK(!served);
    // The owner thread launched, so nothing staged into it is abandoned.
    RUVIA_CHECK(!abandoned.load());

    ruvia::worker_runtime unlaunched;
    std::atomic<int> unlaunched_abandons{};
    unlaunched.configure({.abandon_ = [&] { ++unlaunched_abandons; }});
    unlaunched.request_stop();
    unlaunched.request_stop();
    RUVIA_CHECK_EQ(unlaunched_abandons.load(), 1);
    RUVIA_CHECK(!unlaunched.wait_until_ready());
    unlaunched.join();
}

RUVIA_TEST(worker_runtime_first_failure_wakes_waiters_and_reaches_hook_once) {
    ruvia::worker_runtime runtime;
    std::atomic<int> reports{};
    runtime.configure({
        .startup_ = [&] {
            (void)runtime.fail(std::make_exception_ptr(std::runtime_error("first")));
            (void)runtime.fail(std::make_exception_ptr(std::runtime_error("second"))); },
        .failure_ = [&](std::exception_ptr) noexcept { ++reports; },
    });
    runtime.start();
    std::string message;
    try {
        (void)runtime.wait_until_ready();
    } catch (const std::runtime_error& failure) {
        message = failure.what();
    }
    runtime.join();
    RUVIA_CHECK_EQ(message, std::string("first"));
    RUVIA_CHECK_EQ(reports.load(), 1);
    RUVIA_CHECK_EQ(runtime.state(), state::stopped);
}

RUVIA_TEST(worker_runtime_serve_failure_fails_the_runtime) {
    ruvia::worker_runtime runtime;
    runtime.configure({
        .startup_ = [&] { runtime.mark_ready(); },
        .serve_ = [] { throw std::runtime_error("serve failed"); },
    });
    runtime.start();
    RUVIA_CHECK(runtime.wait_until_ready());
    runtime.request_serve();
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)runtime.wait_until_serving(); }));
    runtime.join();
    RUVIA_CHECK(runtime.failure() != nullptr);
}

RUVIA_TEST(worker_group_orders_tiers_through_startup_serving_and_retirement) {
    ruvia::worker_runtime producer;
    ruvia::worker_runtime first_consumer;
    ruvia::worker_runtime second_consumer;
    std::mutex order_mutex;
    std::vector<std::string> order;
    const auto record = [&](std::string event) {
        const std::lock_guard lock(order_mutex);
        order.push_back(std::move(event));
    };
    std::atomic<bool> staged{};
    std::atomic<bool> consumers_saw_staging{true};
    std::atomic<bool> finalized_after_producer{true};
    producer.configure({
        .startup_ = [&] {
            staged.store(true);
            producer.mark_ready(); },
        .serve_ = [&] { record("serve producer"); },
    });
    for (auto* consumer : {&first_consumer, &second_consumer}) {
        consumer->configure({
            .startup_ = [&, consumer] {
                consumers_saw_staging.store(consumers_saw_staging.load() && staged.load());
                consumer->mark_ready(); },
            .serve_ = [&] { record("serve consumer"); },
            // Consumers wait for the group: their producer must quiesce first.
            .stop_admission_ = [] {},
            .finalize_ = [&] { finalized_after_producer.store(
                                   finalized_after_producer.load() && producer.state() == state::stopped); },
        });
    }
    {
        ruvia::worker_group group;
        group.add(producer, 0);
        group.add(first_consumer, 1);
        group.add(second_consumer, 1);
        RUVIA_CHECK(group.start());
        RUVIA_CHECK(consumers_saw_staging.load());
        RUVIA_CHECK(group.serve());
        group.request_stop();
        RUVIA_CHECK(group.join() == nullptr);
    }
    RUVIA_CHECK(finalized_after_producer.load());
    RUVIA_CHECK_EQ(order, (std::vector<std::string>{"serve consumer", "serve consumer", "serve producer"}));
    RUVIA_CHECK_EQ(producer.state(), state::stopped);
    RUVIA_CHECK_EQ(second_consumer.state(), state::stopped);
}

RUVIA_TEST(worker_group_stop_before_start_launches_nothing) {
    ruvia::worker_runtime member;
    std::atomic<bool> started{};
    member.configure({.startup_ = [&] { started.store(true); }});
    ruvia::worker_group group;
    group.add(member, 0);
    RUVIA_CHECK(ruvia::testing::throws_on([&] { group.add(member, 2); }));
    group.request_stop();
    RUVIA_CHECK(group.stop_requested());
    RUVIA_CHECK(!group.start());
    RUVIA_CHECK(group.join() == nullptr);
    RUVIA_CHECK(!started.load());
    RUVIA_CHECK(!member.started());
}

RUVIA_TEST(worker_group_abandon_hooks_observe_and_reenter_stop) {
    ruvia::worker_runtime first;
    ruvia::worker_runtime second;
    ruvia::worker_group group;
    int abandoned = 0;
    bool observed_stop = true;
    for (auto* member : {&first, &second}) {
        member->configure({.abandon_ = [&] {
            observed_stop = observed_stop && group.stop_requested();
            group.request_stop();
            ++abandoned;
        }});
        group.add(*member, 0);
    }
    group.request_stop();
    group.request_stop();
    RUVIA_CHECK(observed_stop);
    RUVIA_CHECK_EQ(abandoned, 2);
    RUVIA_CHECK(group.join() == nullptr);
    RUVIA_CHECK_EQ(first.state(), state::stopped);
    RUVIA_CHECK_EQ(second.state(), state::stopped);
    RUVIA_CHECK(!first.started() && !second.started());
}

RUVIA_TEST(worker_group_stop_during_startup_retires_unlaunched_members) {
    ruvia::worker_runtime producer;
    ruvia::worker_runtime consumer;
    ruvia::worker_group group;
    std::atomic<bool> abandoned{};
    producer.configure({.startup_ = [&] {
        group.request_stop();
        producer.mark_ready();
    }});
    consumer.configure({.abandon_ = [&] {
        abandoned.store(group.stop_requested());
        group.request_stop();
    }});
    group.add(producer, 0);
    group.add(consumer, 1);
    RUVIA_CHECK(!group.start());
    RUVIA_CHECK(group.join() == nullptr);
    RUVIA_CHECK(abandoned.load());
    RUVIA_CHECK_EQ(producer.state(), state::stopped);
    RUVIA_CHECK_EQ(consumer.state(), state::stopped);
    RUVIA_CHECK(!consumer.started());
}

RUVIA_TEST(worker_group_retirement_waits_for_abandon_dispatch) {
    ruvia::worker_runtime producer;
    ruvia::worker_runtime consumer;
    ruvia::worker_group group;
    std::binary_semaphore abandon_started{0};
    std::binary_semaphore finish_abandon{0};
    std::binary_semaphore join_starting{0};
    std::binary_semaphore producer_retired{0};
    std::atomic<bool> abandon_finished{};
    std::atomic<bool> retired_before_abandon_finished{};
    bool stop_observed = false;
    bool retired_while_blocked = false;
    producer.configure({.shutdown_ = [&] {
        retired_before_abandon_finished.store(!abandon_finished.load());
        producer_retired.release();
    }});
    consumer.configure({.abandon_ = [&] {
        stop_observed = group.stop_requested();
        group.request_stop();
        abandon_started.release();
        finish_abandon.acquire();
        abandon_finished.store(true);
    }});
    group.add(producer, 0);
    group.add(consumer, 1);
    std::thread stopper([&] { group.request_stop(); });
    abandon_started.acquire();
    std::thread observer([&] {
        join_starting.acquire();
        retired_while_blocked = producer_retired.try_acquire_for(std::chrono::milliseconds(500));
        finish_abandon.release();
    });
    join_starting.release();
    const auto failure = group.join();
    stopper.join();
    observer.join();
    RUVIA_CHECK(failure == nullptr);
    RUVIA_CHECK(stop_observed);
    RUVIA_CHECK(abandon_finished.load());
    RUVIA_CHECK(!retired_while_blocked);
    RUVIA_CHECK(!retired_before_abandon_finished.load());
    RUVIA_CHECK_EQ(producer.state(), state::stopped);
    RUVIA_CHECK_EQ(consumer.state(), state::stopped);
}

RUVIA_TEST(worker_group_rethrows_the_first_member_failure) {
    ruvia::worker_runtime healthy;
    ruvia::worker_runtime failing;
    healthy.configure({.startup_ = [&] { healthy.mark_ready(); }});
    failing.configure({.startup_ = [] { throw std::runtime_error("member startup failed"); }});
    ruvia::worker_group group;
    group.add(healthy, 0);
    group.add(failing, 0);
    std::string message;
    try {
        (void)group.start();
    } catch (const std::runtime_error& failure) {
        message = failure.what();
    }
    RUVIA_CHECK_EQ(message, std::string("member startup failed"));
    const auto failure = group.join();
    RUVIA_CHECK(failure != nullptr);
    RUVIA_CHECK_EQ(healthy.state(), state::stopped);
}

ruvia::task<void> hold_retirement(ruvia::worker_signal& gate_value, std::promise<void>& ready) {
    ready.set_value();
    co_await gate_value.wait();
}

struct cleanup_payload final {
    cleanup_payload(const ruvia::worker_handle& worker_value, bool& destroyed)
        : worker_(worker_value),
          destroyed_(destroyed) {}
    ~cleanup_payload() {
        destroyed_ = worker_.is_current();
    }
    const ruvia::worker_handle& worker_;
    bool& destroyed_;
};

RUVIA_TEST(event_loop_cleanup_drains_owned_inputs_after_business_admission_closes) {
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    const auto loop = pool.loop(0);
    const auto worker_value = loop.handle();
    ruvia::worker_signal gate(worker_value);
    std::promise<void> retirement_started;
    auto stop_callback_value = loop.on_stop([&] { return hold_retirement(gate, retirement_started); });
    pool.start();
    pool.stop();
    retirement_started.get_future().get();
    bool ran{};
    bool inputs_destroyed_on_owner{};
    RUVIA_CHECK(!loop.accepting());
    RUVIA_CHECK(loop.defer_cleanup([payload = std::make_unique<cleanup_payload>(worker_value, inputs_destroyed_on_owner), &gate, &ran, &worker_value] {
        ran = worker_value.is_current();
        gate.notify();
    }));
    pool.join();
    RUVIA_CHECK(ran);
    RUVIA_CHECK(inputs_destroyed_on_owner);
    RUVIA_CHECK(!loop.defer_cleanup([] {}));
}

RUVIA_TEST(event_loop_cleanup_can_be_drained_without_starting_business_workers) {
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    const auto loop = pool.loop(0);
    const auto worker_value = loop.handle();
    bool ran{};
    RUVIA_CHECK(loop.defer_cleanup([&] { ran = worker_value.is_current(); }));
    pool.join();
    RUVIA_CHECK(ran);
    RUVIA_CHECK(!loop.valid());
}

RUVIA_TEST(worker_runtime_retains_control_payload_until_owner_completion) {
    ruvia::worker_runtime runtime;
    std::promise<void> initialized;
    std::promise<void> release_owner;
    auto release = release_owner.get_future();
    runtime.configure({.startup_ = [&] {
        initialized.set_value();
        release.wait();
    }});
    runtime.start();
    initialized.get_future().get();
    auto payload_value = std::make_shared<int>(7);
    std::weak_ptr<int> lifetime = payload_value;
    bool consumed{};
    RUVIA_CHECK(runtime.post_control([owned = std::move(payload_value), &consumed] { consumed = *owned == 7; }));
    RUVIA_CHECK(!lifetime.expired());
    runtime.request_stop();
    release_owner.set_value();
    runtime.join();
    RUVIA_CHECK(consumed);
    RUVIA_CHECK(lifetime.expired());
}

}  // namespace
