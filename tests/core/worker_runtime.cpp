#include "ruvia/core/worker_runtime.h"

#include <atomic>
#include <chrono>
#include <exception>
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

using state = ruvia::worker_runtime_state;

RUVIA_TEST(worker_runtime_hook_moves_can_query_failure_during_configuration) {
    ruvia::worker_runtime runtime;
    bool armed = false;
    bool started = false;
    struct hook final {
        ruvia::worker_runtime* runtime_;
        bool* armed_;
        bool* started_;

        hook(ruvia::worker_runtime& runtime_value, bool& armed_value, bool* started_value)
            : runtime_(&runtime_value),
              armed_(&armed_value),
              started_(started_value) {}
        hook(const hook&) = delete;
        hook(hook&& other) noexcept
            : runtime_(other.runtime_),
              armed_(other.armed_),
              started_(other.started_) {
            if (*armed_) {
                (void)runtime_->failure();
            }
        }
        void operator()() {
            if (started_ != nullptr) {
                *started_ = true;
                runtime_->mark_ready();
            } else {
                runtime_->finalize();
            }
        }
    };
    ruvia::worker_runtime_hooks hooks;
    hooks.startup_ = hook(runtime, armed, &started);
    hooks.stop_admission_ = hook(runtime, armed, nullptr);
    armed = true;
    runtime.configure(std::move(hooks));
    RUVIA_CHECK(runtime.start());
    RUVIA_CHECK(runtime.wait_until_ready());
    runtime.request_stop();
    runtime.join();
    RUVIA_CHECK(started);
}

RUVIA_TEST(worker_runtime_drains_accepted_work_before_start) {
    ruvia::worker_runtime runtime;
    const auto worker_value = runtime.handle();
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
    RUVIA_CHECK(!runtime.start());
}

RUVIA_TEST(worker_runtime_cold_stop_cancels_io_and_reclaims_inputs_before_shutdown) {
    ruvia::worker_runtime runtime;
    const auto worker = runtime.handle();
    asio::steady_timer timer(runtime.io_context());
    // 上下文分配的控制块在 worker 释放强引用，在调用线程释放最后的弱引用。
    auto payload = std::allocate_shared<int>(asio::execution_context::allocator<int>(runtime.io_context()), 42);
    std::weak_ptr<int> lifetime = payload;
    bool completion_on_owner = false;
    bool shutdown_after_completion = false;
    timer.expires_after(std::chrono::hours(1));
    timer.async_wait([owned = std::move(payload), &worker, &completion_on_owner](const asio::error_code& error) {
        completion_on_owner = worker.is_current() && error == asio::error::operation_aborted && *owned == 42;
    });
    runtime.configure({
        .stop_admission_ = [&] {
            timer.cancel();
            runtime.finalize(); },
        .shutdown_ = [&] { shutdown_after_completion = worker.is_current() && completion_on_owner && lifetime.expired(); },
    });
    runtime.request_stop();
    RUVIA_CHECK(!worker.accepting());
    RUVIA_CHECK(!runtime.wait_until_ready());
    RUVIA_CHECK(!runtime.wait_until_serving());
    runtime.join();
    RUVIA_CHECK(completion_on_owner);
    RUVIA_CHECK(shutdown_after_completion);
    RUVIA_CHECK(lifetime.expired());
    RUVIA_CHECK_EQ(runtime.state(), state::stopped);
}

RUVIA_TEST(worker_runtime_keeps_owner_until_external_producers_quiesce) {
    ruvia::worker_runtime runtime({.io_policy_ = ruvia::worker_io_policy::single_owner});
    const auto worker_value = runtime.handle();
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
    RUVIA_CHECK(runtime.start());
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
    const auto worker_value = runtime.handle();
    asio::steady_timer timer(runtime.io_context());
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
    RUVIA_CHECK(runtime.start());
    runtime.join();
    RUVIA_CHECK(failure_on_owner);
    RUVIA_CHECK(shutdown_after_completion);
    RUVIA_CHECK_EQ(reported, runtime.failure());
    RUVIA_CHECK_EQ(phases, (std::vector<int>{1, 2, 3, 4}));
    RUVIA_CHECK(runtime.failure() != nullptr);
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)runtime.start(); }));
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
        if (iteration % 2 == 0) {
            RUVIA_CHECK(runtime.start());
        }
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
    const auto worker_value = runtime.handle();
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
    RUVIA_CHECK(runtime.start());
    runtime.join();
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(runtime.state(), state::stopped);
}

RUVIA_TEST(worker_runtime_cold_shutdown_can_repeat_stop_and_rejects_self_join) {
    ruvia::worker_runtime runtime;
    int shutdowns = 0;
    bool self_join_rejected = false;
    bool admission_closed = false;
    runtime.configure({.shutdown_ = [&] {
        ++shutdowns;
        runtime.request_stop();
        admission_closed = !runtime.handle().accepting();
        self_join_rejected = ruvia::testing::throws_on([&] { runtime.join(); });
    }});
    runtime.request_stop();
    runtime.join();
    RUVIA_CHECK_EQ(shutdowns, 1);
    RUVIA_CHECK(self_join_rejected);
    RUVIA_CHECK(admission_closed);
    RUVIA_CHECK_EQ(runtime.state(), state::stopped);
}

RUVIA_TEST(worker_runtime_serves_only_after_owner_published_readiness) {
    ruvia::worker_runtime runtime;
    const auto worker_value = runtime.handle();
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
    RUVIA_CHECK(runtime.start());
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
    bool served{};
    runtime.configure({
        .serve_ = [&] { served = true; },
    });
    RUVIA_CHECK(runtime.start());
    runtime.request_stop();
    RUVIA_CHECK(!runtime.wait_until_ready());
    RUVIA_CHECK(!runtime.wait_until_serving());
    runtime.join();
    RUVIA_CHECK(!served);

    ruvia::worker_runtime unlaunched;
    int shutdowns = 0;
    unlaunched.configure({.shutdown_ = [&] { ++shutdowns; }});
    unlaunched.request_stop();
    unlaunched.request_stop();
    RUVIA_CHECK(!unlaunched.wait_until_ready());
    RUVIA_CHECK(!unlaunched.wait_until_serving());
    unlaunched.join();
    RUVIA_CHECK_EQ(shutdowns, 1);
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
    RUVIA_CHECK(runtime.start());
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
    RUVIA_CHECK(runtime.start());
    RUVIA_CHECK(runtime.wait_until_ready());
    runtime.request_serve();
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)runtime.wait_until_serving(); }));
    runtime.join();
    RUVIA_CHECK(runtime.failure() != nullptr);
}

RUVIA_TEST(worker_group_coordinates_startup_serving_and_retirement) {
    ruvia::worker_runtime first;
    ruvia::worker_runtime second;
    std::mutex order_mutex;
    std::vector<std::string> order;
    const auto record = [&](std::string event) {
        const std::lock_guard lock(order_mutex);
        order.push_back(std::move(event));
    };
    std::atomic<int> ready_count{};
    first.configure({
        .startup_ = [&] {
            ++ready_count;
            first.mark_ready(); },
        .serve_ = [&] { record("serve first"); },
    });
    second.configure({
        .startup_ = [&] {
            ++ready_count;
            second.mark_ready(); },
        .serve_ = [&] { record("serve second"); },
    });
    {
        ruvia::worker_group group;
        group.add(first);
        group.add(second);
        RUVIA_CHECK(group.start());
        RUVIA_CHECK_EQ(ready_count.load(), 2);
        RUVIA_CHECK(group.serve());
        group.request_stop();
        RUVIA_CHECK(group.join() == nullptr);
    }
    RUVIA_CHECK_EQ(order.size(), 2u);
    RUVIA_CHECK_EQ(first.state(), state::stopped);
    RUVIA_CHECK_EQ(second.state(), state::stopped);
}

RUVIA_TEST(worker_group_stop_before_start_launches_nothing) {
    ruvia::worker_runtime member;
    std::atomic<bool> started{};
    member.configure({.startup_ = [&] { started.store(true); }});
    ruvia::worker_group group;
    group.add(member);
    group.request_stop();
    RUVIA_CHECK(group.stop_requested());
    RUVIA_CHECK(!group.start());
    RUVIA_CHECK(group.join() == nullptr);
    RUVIA_CHECK(!started.load());
}

RUVIA_TEST(worker_group_cold_shutdown_hooks_observe_and_reenter_stop) {
    ruvia::worker_runtime first;
    ruvia::worker_runtime second;
    ruvia::worker_group group;
    int shutdowns = 0;
    bool observed_stop = true;
    for (auto* member : {&first, &second}) {
        member->configure({.shutdown_ = [&] {
            observed_stop = observed_stop && group.stop_requested();
            group.request_stop();
            ++shutdowns;
        }});
        group.add(*member);
    }
    group.request_stop();
    group.request_stop();
    RUVIA_CHECK(group.join() == nullptr);
    RUVIA_CHECK(observed_stop);
    RUVIA_CHECK_EQ(shutdowns, 2);
    RUVIA_CHECK_EQ(first.state(), state::stopped);
    RUVIA_CHECK_EQ(second.state(), state::stopped);
}

RUVIA_TEST(worker_group_stop_during_startup_aborts_readiness_and_retires_members) {
    ruvia::worker_runtime first;
    ruvia::worker_runtime second;
    ruvia::worker_group group;
    bool second_served = false;
    bool second_retired = false;
    first.configure({.startup_ = [&] {
        group.request_stop();
        first.mark_ready();
    }});
    second.configure({
        .serve_ = [&] { second_served = true; },
        .shutdown_ = [&] {
            second_retired = group.stop_requested() && second.handle().is_current();
            group.request_stop(); },
    });
    group.add(first);
    group.add(second);
    RUVIA_CHECK(!group.start());
    RUVIA_CHECK(group.join() == nullptr);
    RUVIA_CHECK(!second_served);
    RUVIA_CHECK(second_retired);
    RUVIA_CHECK_EQ(first.state(), state::stopped);
    RUVIA_CHECK_EQ(second.state(), state::stopped);
}

RUVIA_TEST(worker_group_cold_retirement_waits_for_producer_shutdown) {
    ruvia::worker_runtime producer;
    ruvia::worker_runtime consumer;
    ruvia::worker_group group;
    std::binary_semaphore shutdown_started{0};
    std::binary_semaphore finish_shutdown{0};
    std::atomic<bool> shutdown_finished{};
    bool consumer_after_producer = false;
    bool admissions_closed = false;
    producer.configure({.shutdown_ = [&] {
        group.request_stop();
        shutdown_started.release();
        finish_shutdown.acquire();
        shutdown_finished.store(true);
    }});
    consumer.configure({.shutdown_ = [&] {
        consumer_after_producer = shutdown_finished.load() && producer.state() == state::stopped;
    }});
    group.add(producer);
    group.add(consumer);
    std::thread observer([&] {
        shutdown_started.acquire();
        group.request_stop();
        admissions_closed = !producer.handle().accepting() && !consumer.handle().accepting();
        finish_shutdown.release();
    });
    const auto failure = group.join();
    observer.join();
    RUVIA_CHECK(failure == nullptr);
    RUVIA_CHECK(admissions_closed);
    RUVIA_CHECK(consumer_after_producer);
    RUVIA_CHECK_EQ(consumer.state(), state::stopped);
}

RUVIA_TEST(worker_group_external_failure_preserves_cause_while_hook_reenters_stop) {
    ruvia::worker_runtime producer;
    ruvia::worker_runtime consumer;
    ruvia::worker_group group;
    std::binary_semaphore failure_started{0};
    std::binary_semaphore finish_failure{0};
    bool stop_observed = false;
    bool accepted_work_ran = false;
    bool shutdown_on_owner = false;
    const auto error = std::make_exception_ptr(std::runtime_error("external producer failure"));
    producer.configure({
        .failure_ = [&](std::exception_ptr reported) noexcept {
            stop_observed = group.stop_requested() && reported == error;
            group.request_stop();
            failure_started.release();
            finish_failure.acquire(); },
        .shutdown_ = [&] { shutdown_on_owner = producer.handle().is_current(); },
    });
    group.add(producer);
    group.add(consumer);
    RUVIA_CHECK(producer.handle().post([&] { accepted_work_ran = true; }).accepted());
    std::thread reporter([&] { static_cast<void>(producer.fail(error)); });
    failure_started.acquire();
    group.request_stop();
    const bool admissions_closed = !producer.handle().accepting() && !consumer.handle().accepting();
    std::string message;
    try {
        (void)group.start();
    } catch (const std::runtime_error& failure) {
        message = failure.what();
    }
    finish_failure.release();
    reporter.join();
    const auto failure = group.join();
    RUVIA_CHECK(stop_observed);
    RUVIA_CHECK(admissions_closed);
    RUVIA_CHECK_EQ(message, std::string("external producer failure"));
    RUVIA_CHECK_EQ(failure, error);
    RUVIA_CHECK(accepted_work_ran);
    RUVIA_CHECK(shutdown_on_owner);
    RUVIA_CHECK_EQ(consumer.state(), state::stopped);
}

RUVIA_TEST(worker_group_rethrows_the_first_member_failure) {
    ruvia::worker_runtime healthy;
    ruvia::worker_runtime failing;
    healthy.configure({.startup_ = [&] { healthy.mark_ready(); }});
    failing.configure({.startup_ = [] { throw std::runtime_error("member startup failed"); }});
    ruvia::worker_group group;
    group.add(healthy);
    group.add(failing);
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

RUVIA_TEST(worker_group_join_returns_the_callers_primary_failure) {
    ruvia::worker_runtime failing;
    failing.configure({.startup_ = [] { throw std::runtime_error("member startup failed"); }});
    ruvia::worker_group group;
    group.add(failing);
    bool start_threw = false;
    try {
        (void)group.start();
    } catch (const std::runtime_error&) {
        start_threw = true;
    }
    const auto primary = std::make_exception_ptr(std::logic_error("start hook failed"));
    RUVIA_CHECK(start_threw);
    RUVIA_CHECK(group.join(primary) == primary);
    RUVIA_CHECK(group.join() == primary);
    RUVIA_CHECK_EQ(failing.state(), state::stopped);
}

RUVIA_TEST(worker_group_serve_failure_closes_every_members_admission) {
    ruvia::worker_runtime producer;
    ruvia::worker_runtime consumer;
    std::atomic<int> failure_reports{};
    std::atomic<bool> consumer_served{};
    producer.configure({
        .startup_ = [&] { producer.mark_ready(); },
        .serve_ = [&] {
            while (!consumer_served.load()) {
                std::this_thread::yield();
            }
            throw std::runtime_error("producer serve failed"); },
        .failure_ = [&](std::exception_ptr) noexcept { ++failure_reports; },
    });
    consumer.configure({
        .startup_ = [&] { consumer.mark_ready(); },
        .serve_ = [&] { consumer_served.store(true); },
        // The consumer stays stopping until its producer joins.
        .stop_admission_ = [] {},
    });
    ruvia::worker_group group;
    group.add(consumer);
    group.add(producer);
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
    const bool producer_accepting = producer.handle().accepting();
    const bool consumer_accepting = consumer.handle().accepting();
    const bool consumer_accepted_work = consumer.handle().post([] {}).accepted();
    consumer.finalize();
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
        asio::steady_timer startup_timer(pending.io_context());
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
            startup_timer.cancel();
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
        group.add(pending);
        group.add(failing);
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
        const bool pending_accepting = pending.handle().accepting();
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

RUVIA_TEST(worker_group_membership_is_exclusive_and_closes_at_launch_or_stop) {
    ruvia::worker_runtime member;
    ruvia::worker_runtime second;
    ruvia::worker_runtime third;
    member.configure({.startup_ = [&] { member.mark_ready(); }});
    ruvia::worker_group group;
    group.add(member);
    const bool duplicate_rejected = ruvia::testing::throws_on([&] { group.add(member); });
    bool other_group_rejected = false;
    bool stopped_group_rejected = false;
    {
        ruvia::worker_group other;
        other_group_rejected = ruvia::testing::throws_on([&] { other.add(member); });
        other.add(second);
        other.request_stop();
        stopped_group_rejected = ruvia::testing::throws_on([&] { other.add(third); });
        RUVIA_CHECK(other.join() == nullptr);
    }
    const bool started = group.start();
    const bool launched_group_rejected = ruvia::testing::throws_on([&] { group.add(third); });
    const auto failure = group.join();
    const auto repeated_failure = group.join();
    // Rejected additions leave the new member available to its own owner.
    ruvia::worker_group third_group;
    third_group.add(third);
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
        cold_owner.add(member);
    }
    {
        ruvia::worker_group group;
        group.add(member);
        group.add(peer);
        RUVIA_CHECK(group.start());
        RUVIA_CHECK(group.serve());
    }
    const bool members_still_running = member.state() == state::running && peer.state() == state::running;
    const auto error = std::make_exception_ptr(std::runtime_error("independent runtime failure"));
    const bool failure_recorded = member.fail(error);
    const bool peer_accepting = peer.handle().post([] {}).accepted();
    member.join();
    peer.join();
    RUVIA_CHECK(members_still_running);
    RUVIA_CHECK(failure_recorded);
    RUVIA_CHECK(peer_accepting);
    RUVIA_CHECK_EQ(failure_reports.load(), 1);
    RUVIA_CHECK_EQ(member.failure(), error);
}

RUVIA_TEST(worker_group_failure_hook_can_release_completed_member_observations) {
    ruvia::worker_runtime failing;
    ruvia::worker_runtime peer;
    auto group = std::make_unique<ruvia::worker_group>();
    int failure_reports = 0;
    bool stop_observed = false;
    failing.configure({.failure_ = [&](std::exception_ptr) noexcept {
        ++failure_reports;
        stop_observed = group->stop_requested() && !peer.handle().accepting();
        // Member observations have retired before invoking the public hook.
        group.reset();
    }});
    group->add(failing);
    group->add(peer);
    const auto error = std::make_exception_ptr(std::runtime_error("externally reported failure"));
    const bool recorded = failing.fail(error);
    failing.join();
    peer.join();
    RUVIA_CHECK(recorded);
    RUVIA_CHECK(stop_observed);
    RUVIA_CHECK(group == nullptr);
    RUVIA_CHECK_EQ(failure_reports, 1);
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

RUVIA_TEST(event_loop_draining_post_can_schedule_defer_cleanup_after_stop) {
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    const auto loop = pool.loop(0);
    const auto worker_value = loop.handle();
    bool post_ran = false;
    bool cleanup_scheduled = false;
    bool cleanup_ran = false;
    RUVIA_CHECK_EQ(loop.post([loop, worker_value, &post_ran, &cleanup_scheduled, &cleanup_ran] {
        post_ran = worker_value.is_current();
        cleanup_scheduled = loop.defer_cleanup([worker_value, &cleanup_ran] {
            cleanup_ran = worker_value.is_current();
        });
    }),
        ruvia::post_status::accepted);
    pool.stop();
    pool.join();
    RUVIA_CHECK(post_ran);
    RUVIA_CHECK(cleanup_scheduled);
    RUVIA_CHECK(cleanup_ran);
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
    RUVIA_CHECK(runtime.start());
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

RUVIA_TEST(worker_runtime_and_group_reject_repeated_start_and_invalid_serve) {
    {
        ruvia::worker_runtime stopped_runtime;
        stopped_runtime.request_stop();
        RUVIA_CHECK(!stopped_runtime.start());
        RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)stopped_runtime.start(); }));
        stopped_runtime.join();
    }
    {
        ruvia::worker_runtime member;
        member.configure({.startup_ = [&] { member.mark_ready(); }});
        ruvia::worker_group group;
        group.add(member);
        RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)group.serve(); }));
        RUVIA_CHECK(group.start());
        RUVIA_CHECK(group.serve());
        RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)group.serve(); }));
        group.request_stop();
        RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)group.start(); }));
        std::promise<std::exception_ptr> other_joined;
        std::thread joiner([&] { other_joined.set_value(group.join()); });
        RUVIA_CHECK(group.join() == nullptr);
        RUVIA_CHECK(other_joined.get_future().get() == nullptr);
        joiner.join();
    }
}

}  // namespace worker_runtime_test
