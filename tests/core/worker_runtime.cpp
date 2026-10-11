#include "ruvia/core/worker_runtime.h"

#include <atomic>
#include <chrono>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <asio/steady_timer.hpp>

#include "ruvia/core/event_loop_pool.h"
#include "ruvia/core/worker_group.h"
#include "ruvia/core/worker_signal.h"

#include "test_harness.h"

namespace worker_runtime_test {

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

RUVIA_TEST(worker_runtime_abandon_can_repeat_stop_and_rejects_self_join) {
    ruvia::worker_runtime runtime;
    int abandons = 0;
    bool self_join_rejected = false;
    bool admission_closed = false;
    bool shutdown_after_abandon = false;
    runtime.configure({
        .abandon_ = [&] {
            ++abandons;
            runtime.request_stop();
            admission_closed = !runtime.context().handle().accepting();
            try {
                runtime.join();
            } catch (const std::logic_error& error) {
                self_join_rejected = std::string_view(error.what()) == "cannot join a worker runtime while dispatching its stop hooks";
            } },
        .shutdown_ = [&] { shutdown_after_abandon = self_join_rejected && abandons == 1; },
    });
    runtime.request_stop();
    runtime.join();
    RUVIA_CHECK_EQ(abandons, 1);
    RUVIA_CHECK(self_join_rejected);
    RUVIA_CHECK(admission_closed);
    RUVIA_CHECK(shutdown_after_abandon);
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

RUVIA_TEST(worker_group_stop_closes_admission_and_joins_external_abandon_dispatch) {
    ruvia::worker_runtime producer;
    ruvia::worker_runtime consumer;
    ruvia::worker_group group;
    std::binary_semaphore abandon_started{0};
    std::binary_semaphore finish_abandon{0};
    std::binary_semaphore join_starting{0};
    std::binary_semaphore producer_retired{0};
    std::atomic<bool> abandon_finished{};
    std::atomic<bool> work_before_abandon_finished{};
    std::atomic<bool> shutdown_before_abandon_finished{};
    std::atomic<bool> accepted_work_ran{};
    bool retired_while_blocked = false;
    bool watchdog_fired = false;
    producer.configure({
        .abandon_ = [&] {
            // Independent stop must retain its cause even when this hook also
            // asks the group to stop before it releases staged producer state.
            group.request_stop();
            abandon_started.release();
            finish_abandon.acquire();
            abandon_finished.store(true); },
        .shutdown_ = [&] {
            shutdown_before_abandon_finished.store(!abandon_finished.load());
            producer_retired.release(); },
    });
    consumer.configure({.stop_admission_ = [] {}});
    group.add(producer, 0);
    group.add(consumer, 1);
    const auto& worker = producer.context().handle();
    const bool accepted_before_stop = worker.post([&] {
                                                work_before_abandon_finished.store(!abandon_finished.load());
                                                accepted_work_ran.store(true);
                                            })
                                          .accepted();
    std::thread stopper([&] { producer.request_stop(); });
    abandon_started.acquire();
    std::thread observer([&] {
        if (!join_starting.try_acquire_for(std::chrono::seconds(2))) {
            watchdog_fired = true;
        } else {
            retired_while_blocked = producer_retired.try_acquire_for(std::chrono::milliseconds(300));
        }
        finish_abandon.release();
    });
    group.request_stop();
    const bool accepting_after_stop = worker.accepting();
    const bool accepted_after_stop = worker.post([] {}).accepted();
    std::string message;
    bool started = false;
    try {
        started = group.start();
    } catch (const std::exception& failure) {
        message = failure.what();
    }
    const bool accepting_after_phase = worker.accepting();
    join_starting.release();
    const auto failure = group.join();
    stopper.join();
    observer.join();

    RUVIA_CHECK(accepted_before_stop);
    RUVIA_CHECK(!accepting_after_stop && !accepted_after_stop);
    RUVIA_CHECK(!started && !accepting_after_phase);
    RUVIA_CHECK_EQ(message, std::string("worker stopped before the group was ready"));
    RUVIA_CHECK(!watchdog_fired);
    RUVIA_CHECK(!retired_while_blocked);
    RUVIA_CHECK(!work_before_abandon_finished.load());
    RUVIA_CHECK(!shutdown_before_abandon_finished.load());
    RUVIA_CHECK(accepted_work_ran.load());
    RUVIA_CHECK(abandon_finished.load());
    RUVIA_CHECK(failure == nullptr);
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

RUVIA_TEST(worker_group_serve_failure_closes_every_members_admission) {
    ruvia::worker_runtime producer;
    ruvia::worker_runtime consumer;
    std::atomic<int> failure_reports{};
    std::atomic<bool> consumer_served{};
    producer.configure({
        .startup_ = [&] { producer.mark_ready(); },
        .serve_ = [] { throw std::runtime_error("producer serve failed"); },
        .failure_ = [&](std::exception_ptr) noexcept { ++failure_reports; },
    });
    consumer.configure({
        .startup_ = [&] { consumer.mark_ready(); },
        .serve_ = [&] { consumer_served.store(true); },
        // The consumer stays stopping until its producer joins.
        .stop_admission_ = [] {},
    });
    ruvia::worker_group group;
    group.add(producer, 0);
    group.add(consumer, 1);
    const bool started = group.start();
    std::string message;
    try {
        (void)group.serve();
    } catch (const std::runtime_error& failure) {
        message = failure.what();
    }
    const bool group_stopping = group.stop_requested();
    const auto producer_state = producer.state();
    const auto consumer_state = consumer.state();
    const bool producer_accepting = producer.context().handle().accepting();
    const bool consumer_accepting = consumer.context().handle().accepting();
    const bool consumer_accepted_work = consumer.context().handle().post([] {}).accepted();
    const auto failure = group.join();

    RUVIA_CHECK(started);
    RUVIA_CHECK(consumer_served.load());
    RUVIA_CHECK_EQ(message, std::string("producer serve failed"));
    RUVIA_CHECK(group_stopping);
    RUVIA_CHECK(producer_state == state::stopping || producer_state == state::stopped);
    RUVIA_CHECK_EQ(consumer_state, state::stopping);
    RUVIA_CHECK(!producer_accepting && !consumer_accepting);
    RUVIA_CHECK(!consumer_accepted_work);
    RUVIA_CHECK(failure != nullptr);
    RUVIA_CHECK_EQ(failure_reports.load(), 1);
    RUVIA_CHECK_EQ(producer.state(), state::stopped);
    RUVIA_CHECK_EQ(consumer.state(), state::stopped);
}

RUVIA_TEST(worker_group_member_exit_wakes_an_earlier_asynchronous_startup) {
    for (const bool with_failure : {false, true}) {
        ruvia::worker_runtime pending;
        ruvia::worker_runtime failing;
        asio::steady_timer startup_timer(pending.context().io_context());
        std::promise<void> startup_waiting;
        auto startup_waiting_future = startup_waiting.get_future();
        std::atomic<bool> startup_cancelled{};
        std::atomic<int> failure_reports{};
        std::weak_ptr<int> weak_payload;
        pending.configure({
            .startup_ = [&] {
            const auto payload = std::make_shared<int>(42);
            weak_payload = payload;
            startup_timer.expires_after(std::chrono::hours(1));
            startup_timer.async_wait([&, payload](const asio::error_code& error) {
                startup_cancelled.store(error == asio::error::operation_aborted && *payload == 42);
                if (!error) {
                    pending.mark_ready();
                }
            });
            startup_waiting.set_value(); },
            .stop_admission_ = [&] {
            asio::error_code ignored;
            startup_timer.cancel(ignored);
            pending.finalize(); },
        });
        failing.configure({
            .startup_ = [&] {
            startup_waiting_future.wait();
            if (with_failure) {
                throw std::runtime_error("peer startup failed");
            }
            failing.request_stop(); },
            .failure_ = [&](std::exception_ptr) noexcept { ++failure_reports; },
        });
        ruvia::worker_group group;
        group.add(pending, 0);
        group.add(failing, 0);
        std::promise<void> start_returned;
        auto start_returned_future = start_returned.get_future();
        std::atomic<bool> watchdog_fired{};
        std::thread watchdog([&] {
            if (start_returned_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
                watchdog_fired.store(true);
                group.request_stop();
            }
        });
        std::string message;
        bool started = false;
        try {
            started = group.start();
        } catch (const std::runtime_error& failure) {
            message = failure.what();
        }
        start_returned.set_value();
        watchdog.join();
        const bool group_stopping = group.stop_requested();
        const bool pending_accepting = pending.context().handle().accepting();
        const auto failure = group.join();

        RUVIA_CHECK(!watchdog_fired.load());
        RUVIA_CHECK(!started);
        RUVIA_CHECK_EQ(message, std::string(with_failure ? "peer startup failed" : "worker stopped before the group was ready"));
        RUVIA_CHECK(group_stopping);
        RUVIA_CHECK(!pending_accepting);
        RUVIA_CHECK_EQ(failure != nullptr, with_failure);
        RUVIA_CHECK(startup_cancelled.load());
        RUVIA_CHECK(weak_payload.expired());
        RUVIA_CHECK_EQ(failure_reports.load(), with_failure ? 1 : 0);
        RUVIA_CHECK_EQ(pending.state(), state::stopped);
        RUVIA_CHECK_EQ(failing.state(), state::stopped);
    }
}

RUVIA_TEST(worker_group_tiers_do_not_wrap_at_the_maximum_value) {
    ruvia::worker_runtime first;
    ruvia::worker_runtime second;
    ruvia::worker_group group;
    group.add(first, std::numeric_limits<std::size_t>::max());
    bool rejected = false;
    try {
        group.add(second, 0);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    const auto failure = group.join();
    RUVIA_CHECK(rejected);
    RUVIA_CHECK(failure == nullptr);
}

RUVIA_TEST(worker_group_membership_is_exclusive_and_closes_at_launch_or_stop) {
    ruvia::worker_runtime member;
    ruvia::worker_runtime second;
    ruvia::worker_runtime third;
    member.configure({.startup_ = [&] { member.mark_ready(); }});
    ruvia::worker_group group;
    group.add(member, 0);
    const bool duplicate_rejected = ruvia::testing::throws_on([&] { group.add(member, 0); });
    bool other_group_rejected = false;
    bool stopped_group_rejected = false;
    {
        ruvia::worker_group other;
        other_group_rejected = ruvia::testing::throws_on([&] { other.add(member, 0); });
        other.add(second, 0);
        other.request_stop();
        stopped_group_rejected = ruvia::testing::throws_on([&] { other.add(third, 0); });
        RUVIA_CHECK(other.join() == nullptr);
    }
    const bool started = group.start();
    const bool launched_group_rejected = ruvia::testing::throws_on([&] { group.add(third, 0); });
    const auto failure = group.join();
    const auto repeated_failure = group.join();
    // Rejected additions leave the new member available to its own owner.
    ruvia::worker_group third_group;
    third_group.add(third, 0);
    RUVIA_CHECK(third_group.join() == nullptr);
    RUVIA_CHECK(duplicate_rejected);
    RUVIA_CHECK(other_group_rejected);
    RUVIA_CHECK(stopped_group_rejected);
    RUVIA_CHECK(started);
    RUVIA_CHECK(launched_group_rejected);
    RUVIA_CHECK(failure == nullptr && repeated_failure == failure);
}

RUVIA_TEST(worker_group_destruction_releases_observations_and_preserves_runtime_owners) {
    ruvia::worker_runtime member;
    ruvia::worker_runtime peer;
    std::atomic<int> failure_reports{};
    member.configure({
        .startup_ = [&] { member.mark_ready(); },
        .failure_ = [&](std::exception_ptr) noexcept { ++failure_reports; },
    });
    peer.configure({.startup_ = [&] { peer.mark_ready(); }});
    {
        ruvia::worker_group cold_owner;
        cold_owner.add(member, 0);
    }
    {
        ruvia::worker_group group;
        group.add(member, 0);
        group.add(peer, 0);
        RUVIA_CHECK(group.start());
        RUVIA_CHECK(group.serve());
    }
    const bool members_still_running = member.state() == state::running && peer.state() == state::running;
    const auto error = std::make_exception_ptr(std::runtime_error("independent runtime failure"));
    const bool failure_recorded = member.fail(error);
    const bool peer_accepting = peer.context().handle().post([] {}).accepted();
    member.join();
    peer.join();
    RUVIA_CHECK(members_still_running);
    RUVIA_CHECK(failure_recorded);
    RUVIA_CHECK(peer_accepting);
    RUVIA_CHECK_EQ(failure_reports.load(), 1);
    RUVIA_CHECK_EQ(member.failure(), error);
}

RUVIA_TEST(worker_group_destruction_waits_for_inflight_member_notifications) {
    ruvia::worker_runtime failing;
    ruvia::worker_runtime peer;
    std::promise<void> abandon_started;
    auto abandon_started_future = abandon_started.get_future();
    std::promise<void> release_abandon;
    auto release_abandon_future = release_abandon.get_future();
    std::atomic<bool> abandon_finished{};
    std::atomic<int> failure_reports{};
    failing.configure({.failure_ = [&](std::exception_ptr) noexcept { ++failure_reports; }});
    peer.configure({.abandon_ = [&] {
        abandon_started.set_value();
        release_abandon_future.wait();
        abandon_finished.store(true);
    }});
    auto group = std::make_unique<ruvia::worker_group>();
    group->add(failing, 0);
    group->add(peer, 0);
    const auto error = std::make_exception_ptr(std::runtime_error("externally reported failure"));
    std::thread reporter([&] { static_cast<void>(failing.fail(error)); });
    const bool notification_inflight = abandon_started_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    std::promise<void> destruction_returned;
    auto destruction_returned_future = destruction_returned.get_future();
    bool returned_while_blocked = false;
    std::thread unblocker([&] {
        if (notification_inflight) {
            returned_while_blocked = destruction_returned_future.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready;
        }
        release_abandon.set_value();
    });
    group.reset();
    const bool abandonment_finished_at_destruction = abandon_finished.load();
    destruction_returned.set_value();
    unblocker.join();
    reporter.join();
    failing.join();
    peer.join();
    RUVIA_CHECK(notification_inflight);
    RUVIA_CHECK(!returned_while_blocked);
    RUVIA_CHECK(abandonment_finished_at_destruction);
    RUVIA_CHECK_EQ(failure_reports.load(), 1);
    RUVIA_CHECK_EQ(failing.failure(), error);
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

}  // namespace worker_runtime_test
