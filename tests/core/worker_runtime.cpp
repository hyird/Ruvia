#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_ref.h"

#include "test_exceptions.h"
#include "worker_task_fixture.h"

#ifndef _WIN32
#include <unistd.h>
#endif

namespace worker_runtime_test {

using state = ruvia::worker_runtime_state;

// take_failure() marks it observed, so it is not offered on a const runtime.
template <typename runtime_type>
concept failure_readable_through_const = requires(const runtime_type& runtime_value) { runtime_value.take_failure(); };
static_assert(!failure_readable_through_const<ruvia::worker_runtime>);
static_assert(requires(ruvia::worker_runtime& runtime_value) { runtime_value.take_failure(); });

TEST(Ruvia, worker_runtime_hook_moves_can_query_failure_during_configuration) {
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
                (void)runtime_->take_failure();
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
    EXPECT_TRUE(runtime.start());
    EXPECT_TRUE(runtime.wait_until_ready());
    runtime.request_stop();
    runtime.join();
    EXPECT_TRUE(started);
}

TEST(Ruvia, worker_runtime_finalize_hook_move_can_reenter_finalize) {
    ruvia::worker_runtime runtime;
    bool armed = false;
    bool invoked = false;
    struct hook final {
        ruvia::worker_runtime* runtime_;
        bool* armed_;
        bool* invoked_;

        hook(ruvia::worker_runtime& owner, bool& armed_value, bool& invoked_value)
            : runtime_(&owner),
              armed_(&armed_value),
              invoked_(&invoked_value) {}
        hook(const hook&) = delete;
        hook(hook&& other) noexcept
            : runtime_(other.runtime_),
              armed_(other.armed_),
              invoked_(other.invoked_) {
            if (*armed_) {
                runtime_->finalize();
            }
        }
        ~hook() {
            if (*armed_) {
                runtime_->finalize();
            }
        }
        void operator()() {
            *invoked_ = true;
        }
    };
    runtime.configure({.finalize_ = hook(runtime, armed, invoked)});
    armed = true;
    runtime.finalize();
    runtime.join();
    EXPECT_TRUE(invoked);
}

TEST(Ruvia, worker_runtime_drains_accepted_work_before_start) {
    ruvia::worker_runtime runtime;
    const auto worker_value = runtime.worker();
    const auto caller = std::this_thread::get_id();
    bool owner_affine{};
    bool ran{};
    EXPECT_TRUE(worker_value.post([&] {
                                owner_affine = worker_value.is_current() && std::this_thread::get_id() != caller;
                                ran = true;
                            })
            .accepted());
    runtime.join();
    EXPECT_TRUE(ran);
    EXPECT_TRUE(owner_affine);
    EXPECT_EQ(runtime.state(), state::stopped);
    EXPECT_TRUE(!worker_value.valid());
    EXPECT_TRUE(!worker_value.post([] {}).accepted());
    EXPECT_TRUE(!runtime.start());
}

TEST(Ruvia, worker_runtime_cold_stop_cancels_io_and_reclaims_inputs_before_shutdown) {
    ruvia::worker_runtime runtime;
    const auto worker = runtime.worker();
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
    EXPECT_TRUE(!worker.accepting());
    EXPECT_TRUE(!runtime.wait_until_ready());
    EXPECT_TRUE(!runtime.wait_until_serving());
    runtime.join();
    EXPECT_TRUE(completion_on_owner);
    EXPECT_TRUE(shutdown_after_completion);
    EXPECT_TRUE(lifetime.expired());
    EXPECT_EQ(runtime.state(), state::stopped);
}

TEST(Ruvia, worker_runtime_keeps_owner_until_external_producers_quiesce) {
    ruvia::worker_runtime runtime({.io_policy_ = ruvia::worker_io_policy::single_owner});
    const auto worker_value = runtime.worker();
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
    EXPECT_TRUE(runtime.start());
    ready.get_future().get();
    runtime.request_stop();
    admission_stopped.get_future().get();
    EXPECT_TRUE(stopped_on_owner.load());
    EXPECT_EQ(runtime.state(), state::stopping);
    EXPECT_TRUE(worker_value.valid());
    EXPECT_TRUE(!worker_value.accepting());
    EXPECT_TRUE(!runtime.post_control([] {}));
    runtime.finalize();
    runtime.join();
    EXPECT_TRUE(finalized_on_owner);
    EXPECT_TRUE(shutdown_on_owner);
    EXPECT_EQ(phases, (std::vector<int>{1, 2, 3, 4}));
    EXPECT_TRUE(!worker_value.valid());
}

TEST(Ruvia, worker_runtime_failure_cancels_then_drains_io_before_shutdown) {
    ruvia::worker_runtime runtime;
    const auto worker_value = runtime.worker();
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
    EXPECT_TRUE(runtime.start());
    runtime.join();
    EXPECT_TRUE(failure_on_owner);
    EXPECT_TRUE(shutdown_after_completion);
    EXPECT_EQ(reported, runtime.take_failure());
    EXPECT_EQ(phases, (std::vector<int>{1, 2, 3, 4}));
    EXPECT_TRUE(runtime.take_failure() != nullptr);
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)runtime.start(); }));
}

TEST(Ruvia, worker_runtime_concurrent_stop_and_join_share_one_barrier) {
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
            EXPECT_TRUE(runtime.start());
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
        EXPECT_EQ(stops.load(), 1);
        EXPECT_EQ(shutdowns.load(), 1);
        EXPECT_EQ(runtime.state(), state::stopped);
    }
}

TEST(Ruvia, worker_runtime_finalize_before_start_retires_on_owner) {
    ruvia::worker_runtime runtime;
    const auto worker_value = runtime.worker();
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
    EXPECT_TRUE(!startup_ran);
    EXPECT_TRUE(stop_ran);
    EXPECT_TRUE(finalizer_ran);
    EXPECT_TRUE(shutdown_ran);
}

TEST(Ruvia, worker_runtime_rejects_self_join_without_losing_cleanup) {
    ruvia::worker_runtime runtime;
    bool rejected{};
    runtime.configure({
        .startup_ = [&] { rejected = ruvia::test::throws_on([&] { runtime.join(); }); },
    });
    EXPECT_TRUE(runtime.start());
    runtime.join();
    EXPECT_TRUE(rejected);
    EXPECT_EQ(runtime.state(), state::stopped);
}

TEST(Ruvia, worker_runtime_cold_shutdown_can_repeat_stop_and_rejects_self_join) {
    ruvia::worker_runtime runtime;
    int shutdowns = 0;
    bool self_join_rejected = false;
    bool admission_closed = false;
    runtime.configure({.shutdown_ = [&] {
        ++shutdowns;
        runtime.request_stop();
        admission_closed = !runtime.worker().accepting();
        self_join_rejected = ruvia::test::throws_on([&] { runtime.join(); });
    }});
    runtime.request_stop();
    runtime.join();
    EXPECT_EQ(shutdowns, 1);
    EXPECT_TRUE(self_join_rejected);
    EXPECT_TRUE(admission_closed);
    EXPECT_EQ(runtime.state(), state::stopped);
}

TEST(Ruvia, worker_runtime_serves_only_after_owner_published_readiness) {
    ruvia::worker_runtime runtime;
    const auto worker_value = runtime.worker();
    std::atomic<bool> served_on_owner{};
    std::atomic<int> serves{};
    runtime.configure({
        .startup_ = [&] {
            // Readiness may follow asynchronous startup work on the owner.
            EXPECT_TRUE(worker_value.post([&] { runtime.mark_ready(); }).accepted()); },
        .serve_ = [&] {
            ++serves;
            served_on_owner.store(worker_value.is_current()); },
    });
    EXPECT_TRUE(runtime.start());
    EXPECT_TRUE(runtime.wait_until_ready());
    EXPECT_EQ(serves.load(), 0);
    runtime.request_serve();
    runtime.request_serve();
    EXPECT_TRUE(runtime.wait_until_serving());
    EXPECT_EQ(serves.load(), 1);
    EXPECT_TRUE(served_on_owner.load());
    runtime.join();
    EXPECT_TRUE(runtime.take_failure() == nullptr);
}

TEST(Ruvia, worker_runtime_stop_before_readiness_aborts_startup) {
    ruvia::worker_runtime runtime;
    bool served{};
    runtime.configure({
        .serve_ = [&] { served = true; },
    });
    EXPECT_TRUE(runtime.start());
    runtime.request_stop();
    EXPECT_TRUE(!runtime.wait_until_ready());
    EXPECT_TRUE(!runtime.wait_until_serving());
    runtime.join();
    EXPECT_TRUE(!served);

    ruvia::worker_runtime unlaunched;
    int shutdowns = 0;
    unlaunched.configure({.shutdown_ = [&] { ++shutdowns; }});
    unlaunched.request_stop();
    unlaunched.request_stop();
    EXPECT_TRUE(!unlaunched.wait_until_ready());
    EXPECT_TRUE(!unlaunched.wait_until_serving());
    unlaunched.join();
    EXPECT_EQ(shutdowns, 1);
}

TEST(Ruvia, worker_runtime_first_failure_wakes_waiters_and_reaches_hook_once) {
    ruvia::worker_runtime runtime;
    std::atomic<int> reports{};
    runtime.configure({
        .startup_ = [&] {
            (void)runtime.fail(std::make_exception_ptr(std::runtime_error("first")));
            (void)runtime.fail(std::make_exception_ptr(std::runtime_error("second"))); },
        .failure_ = [&](std::exception_ptr) noexcept { ++reports; },
    });
    EXPECT_TRUE(runtime.start());
    std::string message;
    try {
        (void)runtime.wait_until_ready();
    } catch (const std::runtime_error& failure) {
        message = failure.what();
    }
    runtime.join();
    EXPECT_EQ(message, "first");
    EXPECT_EQ(reports.load(), 1);
    EXPECT_EQ(runtime.state(), state::stopped);
}

TEST(Ruvia, worker_runtime_serve_failure_fails_the_runtime) {
    ruvia::worker_runtime runtime;
    runtime.configure({
        .startup_ = [&] { runtime.mark_ready(); },
        .serve_ = [] { throw std::runtime_error("serve failed"); },
    });
    EXPECT_TRUE(runtime.start());
    EXPECT_TRUE(runtime.wait_until_ready());
    runtime.request_serve();
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)runtime.wait_until_serving(); }));
    runtime.join();
    EXPECT_TRUE(runtime.take_failure() != nullptr);
}

TEST(Ruvia, worker_group_coordinates_startup_serving_and_retirement) {
    ruvia::worker_runtime first;
    ruvia::worker_runtime second;
    std::mutex order_mutex;
    std::vector<std::string> order;
    const auto record = [&](std::string_view event) {
        const std::lock_guard lock(order_mutex);
        order.emplace_back(event);
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
        EXPECT_TRUE(group.start());
        EXPECT_EQ(ready_count.load(), 2);
        EXPECT_TRUE(group.serve());
        group.request_stop();
        EXPECT_TRUE(group.join() == nullptr);
    }
    EXPECT_EQ(order.size(), 2u);
    EXPECT_EQ(first.state(), state::stopped);
    EXPECT_EQ(second.state(), state::stopped);
}

TEST(Ruvia, worker_group_stop_before_start_launches_nothing) {
    ruvia::worker_runtime member;
    std::atomic<bool> started{};
    member.configure({.startup_ = [&] { started.store(true); }});
    ruvia::worker_group group;
    group.add(member);
    group.request_stop();
    EXPECT_TRUE(group.stop_requested());
    EXPECT_TRUE(!group.start());
    EXPECT_TRUE(group.join() == nullptr);
    EXPECT_TRUE(!started.load());
}

TEST(Ruvia, worker_group_cold_shutdown_hooks_observe_and_reenter_stop) {
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
    EXPECT_TRUE(group.join() == nullptr);
    EXPECT_TRUE(observed_stop);
    EXPECT_EQ(shutdowns, 2);
    EXPECT_EQ(first.state(), state::stopped);
    EXPECT_EQ(second.state(), state::stopped);
}

TEST(Ruvia, worker_group_stop_during_startup_aborts_readiness_and_retires_members) {
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
            second_retired = group.stop_requested() && second.worker().is_current();
            group.request_stop(); },
    });
    group.add(first);
    group.add(second);
    EXPECT_TRUE(!group.start());
    EXPECT_TRUE(group.join() == nullptr);
    EXPECT_TRUE(!second_served);
    EXPECT_TRUE(second_retired);
    EXPECT_EQ(first.state(), state::stopped);
    EXPECT_EQ(second.state(), state::stopped);
}

TEST(Ruvia, worker_group_cold_retirement_waits_for_producer_shutdown) {
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
        admissions_closed = !producer.worker().accepting() && !consumer.worker().accepting();
        finish_shutdown.release();
    });
    const auto failure = group.join();
    observer.join();
    EXPECT_TRUE(failure == nullptr);
    EXPECT_TRUE(admissions_closed);
    EXPECT_TRUE(consumer_after_producer);
    EXPECT_EQ(consumer.state(), state::stopped);
}

TEST(Ruvia, worker_group_external_failure_preserves_cause_while_hook_reenters_stop) {
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
        .shutdown_ = [&] { shutdown_on_owner = producer.worker().is_current(); },
    });
    group.add(producer);
    group.add(consumer);
    EXPECT_TRUE(producer.worker().post([&] { accepted_work_ran = true; }).accepted());
    std::thread reporter([&] { static_cast<void>(producer.fail(error)); });
    failure_started.acquire();
    group.request_stop();
    const bool admissions_closed = !producer.worker().accepting() && !consumer.worker().accepting();
    std::string message;
    try {
        (void)group.start();
    } catch (const std::runtime_error& failure) {
        message = failure.what();
    }
    finish_failure.release();
    reporter.join();
    const auto failure = group.join();
    EXPECT_TRUE(stop_observed);
    EXPECT_TRUE(admissions_closed);
    EXPECT_EQ(message, "external producer failure");
    EXPECT_EQ(failure, error);
    EXPECT_TRUE(accepted_work_ran);
    EXPECT_TRUE(shutdown_on_owner);
    EXPECT_EQ(consumer.state(), state::stopped);
}

TEST(Ruvia, worker_group_rethrows_the_first_member_failure) {
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
    EXPECT_EQ(message, "member startup failed");
    const auto failure = group.join();
    EXPECT_TRUE(failure != nullptr);
    EXPECT_EQ(healthy.state(), state::stopped);
}

TEST(Ruvia, worker_group_join_returns_the_callers_primary_failure) {
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
    EXPECT_TRUE(start_threw);
    EXPECT_TRUE(group.join(primary) == primary);
    EXPECT_TRUE(group.join() == primary);
    EXPECT_EQ(failing.state(), state::stopped);
}

TEST(Ruvia, worker_group_serve_failure_closes_every_members_admission) {
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
    const bool producer_accepting = producer.worker().accepting();
    const bool consumer_accepting = consumer.worker().accepting();
    const bool consumer_accepted_work = consumer.worker().post([] {}).accepted();
    consumer.finalize();
    const auto failure = group.join();

    EXPECT_TRUE(started);
    EXPECT_TRUE(consumer_served.load());
    EXPECT_EQ(message, "producer serve failed");
    EXPECT_TRUE(group_stopping);
    EXPECT_TRUE(producer_state == state::stopping || producer_state == state::stopped);
    EXPECT_EQ(consumer_state, state::stopping);
    EXPECT_TRUE(!producer_accepting && !consumer_accepting);
    EXPECT_TRUE(!consumer_accepted_work);
    EXPECT_TRUE(failure != nullptr);
    EXPECT_EQ(failure_reports.load(), 1);
    EXPECT_EQ(producer.state(), state::stopped);
    EXPECT_EQ(consumer.state(), state::stopped);
}

TEST(Ruvia, worker_group_member_exit_wakes_an_earlier_asynchronous_startup) {
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
        const bool pending_accepting = pending.worker().accepting();
        const auto failure = group.join();

        EXPECT_TRUE(!watchdog_fired.load());
        EXPECT_TRUE(!started);
        EXPECT_EQ(message, with_failure ? "peer startup failed" : "worker stopped before the group was ready");
        EXPECT_TRUE(group_stopping);
        EXPECT_TRUE(!pending_accepting);
        EXPECT_EQ(failure != nullptr, with_failure);
        EXPECT_TRUE(startup_cancelled.load());
        EXPECT_TRUE(weak_payload.expired());
        EXPECT_EQ(failure_reports.load(), with_failure ? 1 : 0);
        EXPECT_EQ(pending.state(), state::stopped);
        EXPECT_EQ(failing.state(), state::stopped);
    }
}

TEST(Ruvia, worker_group_membership_is_exclusive_and_closes_at_launch_or_stop) {
    ruvia::worker_runtime member;
    ruvia::worker_runtime second;
    ruvia::worker_runtime third;
    member.configure({.startup_ = [&] { member.mark_ready(); }});
    ruvia::worker_group group;
    group.add(member);
    const bool duplicate_rejected = ruvia::test::throws_on([&] { group.add(member); });
    bool other_group_rejected = false;
    bool stopped_group_rejected = false;
    {
        ruvia::worker_group other;
        other_group_rejected = ruvia::test::throws_on([&] { other.add(member); });
        other.add(second);
        other.request_stop();
        stopped_group_rejected = ruvia::test::throws_on([&] { other.add(third); });
        EXPECT_TRUE(other.join() == nullptr);
    }
    const bool started = group.start();
    const bool launched_group_rejected = ruvia::test::throws_on([&] { group.add(third); });
    const auto failure = group.join();
    const auto repeated_failure = group.join();
    // Rejected additions leave the new member available to its own owner.
    ruvia::worker_group third_group;
    third_group.add(third);
    EXPECT_TRUE(third_group.join() == nullptr);
    EXPECT_TRUE(duplicate_rejected);
    EXPECT_TRUE(other_group_rejected);
    EXPECT_TRUE(stopped_group_rejected);
    EXPECT_TRUE(started);
    EXPECT_TRUE(launched_group_rejected);
    EXPECT_TRUE(failure == nullptr && repeated_failure == failure);
}

TEST(Ruvia, worker_group_destruction_releases_observations_and_preserves_runtime_owners) {
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
        EXPECT_TRUE(group.start());
        EXPECT_TRUE(group.serve());
    }
    const bool members_still_running = member.state() == state::running && peer.state() == state::running;
    const auto error = std::make_exception_ptr(std::runtime_error("independent runtime failure"));
    const bool failure_recorded = member.fail(error);
    const bool peer_accepting = peer.worker().post([] {}).accepted();
    member.join();
    peer.join();
    EXPECT_TRUE(members_still_running);
    EXPECT_TRUE(failure_recorded);
    EXPECT_TRUE(peer_accepting);
    EXPECT_EQ(failure_reports.load(), 1);
    EXPECT_EQ(member.take_failure(), error);
}

TEST(Ruvia, worker_group_failure_hook_can_release_completed_member_observations) {
    ruvia::worker_runtime failing;
    ruvia::worker_runtime peer;
    auto group = std::make_unique<ruvia::worker_group>();
    int failure_reports = 0;
    bool stop_observed = false;
    failing.configure({.failure_ = [&](std::exception_ptr) noexcept {
        ++failure_reports;
        stop_observed = group->stop_requested() && !peer.worker().accepting();
        // Member observations have retired before invoking the public hook.
        group.reset();
    }});
    group->add(failing);
    group->add(peer);
    const auto error = std::make_exception_ptr(std::runtime_error("externally reported failure"));
    const bool recorded = failing.fail(error);
    failing.join();
    peer.join();
    EXPECT_TRUE(recorded);
    EXPECT_TRUE(stop_observed);
    EXPECT_TRUE(group == nullptr);
    EXPECT_EQ(failure_reports, 1);
    EXPECT_EQ(failing.take_failure(), error);
}

ruvia::task<void> hold_retirement(ruvia::worker_signal& gate_value, std::promise<void>& ready) {
    ready.set_value();
    co_await gate_value.wait();
}

struct cleanup_payload final {
    cleanup_payload(const ruvia::worker_ref& worker_value, bool& destroyed)
        : worker_(worker_value),
          destroyed_(destroyed) {}
    ~cleanup_payload() {
        destroyed_ = worker_.is_current();
    }
    const ruvia::worker_ref& worker_;
    bool& destroyed_;
};

TEST(Ruvia, event_loop_cleanup_drains_owned_inputs_after_business_admission_closes) {
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    const auto loop = pool.loop(0);
    const auto worker_value = loop.worker();
    ruvia::worker_signal gate(worker_value);
    std::promise<void> retirement_started;
    auto stop_callback_value = loop.on_stop([&] { return hold_retirement(gate, retirement_started); });
    pool.start();
    pool.stop();
    retirement_started.get_future().get();
    bool ran{};
    bool inputs_destroyed_on_owner{};
    EXPECT_TRUE(!loop.worker().accepting());
    EXPECT_TRUE(loop.defer_cleanup([payload = std::make_unique<cleanup_payload>(worker_value, inputs_destroyed_on_owner), &gate, &ran, &worker_value] {
        ran = worker_value.is_current();
        gate.notify();
    }));
    pool.join();
    EXPECT_TRUE(ran);
    EXPECT_TRUE(inputs_destroyed_on_owner);
    EXPECT_TRUE(!loop.defer_cleanup([] {}));
}

TEST(Ruvia, event_loop_cleanup_can_be_drained_without_starting_business_workers) {
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    const auto loop = pool.loop(0);
    const auto worker_value = loop.worker();
    bool ran{};
    EXPECT_TRUE(loop.defer_cleanup([&] { ran = worker_value.is_current(); }));
    pool.join();
    EXPECT_TRUE(ran);
    EXPECT_TRUE(!loop.worker().valid());
}

TEST(Ruvia, event_loop_draining_post_can_schedule_defer_cleanup_after_stop) {
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    const auto loop = pool.loop(0);
    const auto worker_value = loop.worker();
    bool post_ran = false;
    bool cleanup_scheduled = false;
    bool cleanup_ran = false;
    EXPECT_EQ(loop.worker().post([loop, worker_value, &post_ran, &cleanup_scheduled, &cleanup_ran] {
        post_ran = worker_value.is_current();
        cleanup_scheduled = loop.defer_cleanup([worker_value, &cleanup_ran] {
            cleanup_ran = worker_value.is_current();
        });
    }),
        ruvia::post_status::accepted);
    pool.stop();
    pool.join();
    EXPECT_TRUE(post_ran);
    EXPECT_TRUE(cleanup_scheduled);
    EXPECT_TRUE(cleanup_ran);
}

TEST(Ruvia, worker_runtime_retains_control_payload_until_owner_completion) {
    ruvia::worker_runtime runtime;
    std::promise<void> initialized;
    std::promise<void> release_owner;
    auto release = release_owner.get_future();
    runtime.configure({.startup_ = [&] {
        initialized.set_value();
        release.wait();
    }});
    EXPECT_TRUE(runtime.start());
    initialized.get_future().get();
    auto payload_value = std::make_shared<int>(7);
    std::weak_ptr<int> lifetime = payload_value;
    bool consumed{};
    EXPECT_TRUE(runtime.post_control([owned = std::move(payload_value), &consumed] { consumed = *owned == 7; }));
    EXPECT_TRUE(!lifetime.expired());
    runtime.request_stop();
    release_owner.set_value();
    runtime.join();
    EXPECT_TRUE(consumed);
    EXPECT_TRUE(lifetime.expired());
}

TEST(Ruvia, worker_runtime_and_group_reject_repeated_start_and_invalid_serve) {
    {
        ruvia::worker_runtime stopped_runtime;
        stopped_runtime.request_stop();
        EXPECT_TRUE(!stopped_runtime.start());
        EXPECT_TRUE(ruvia::test::throws_on([&] { (void)stopped_runtime.start(); }));
        stopped_runtime.join();
    }
    {
        ruvia::worker_runtime member;
        member.configure({.startup_ = [&] { member.mark_ready(); }});
        ruvia::worker_group group;
        group.add(member);
        EXPECT_TRUE(ruvia::test::throws_on([&] { (void)group.serve(); }));
        EXPECT_TRUE(group.start());
        EXPECT_TRUE(group.serve());
        EXPECT_TRUE(ruvia::test::throws_on([&] { (void)group.serve(); }));
        group.request_stop();
        EXPECT_TRUE(ruvia::test::throws_on([&] { (void)group.start(); }));
        std::promise<std::exception_ptr> other_joined;
        std::thread joiner([&] { other_joined.set_value(group.join()); });
        EXPECT_TRUE(group.join() == nullptr);
        EXPECT_TRUE(other_joined.get_future().get() == nullptr);
        joiner.join();
    }
}

ruvia::task<int> sleep_then_finish(ruvia::worker_ref worker_value, std::atomic_bool& finished) {
    // Worker shutdown cuts the sleep short; the root still finishes normally.
    static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::seconds(30)));
    finished = true;
    co_return 11;
}

TEST(Ruvia, WorkerRuntimeLoopGatesStopAdmissionOnItsRoots) {
    ruvia::worker_runtime runtime;
    std::atomic_bool root_finished{false};
    std::atomic_bool admitted_after_root{false};
    std::atomic_bool admission_ran{false};
    runtime.configure({
        .startup_ = [&] { runtime.mark_ready(); },
        .stop_admission_ =
            [&] {
                admitted_after_root = root_finished.load();
                admission_ran = true;
                runtime.finalize();
            },
    });
    const auto loop = runtime.loop();
    EXPECT_TRUE(loop.valid());
    EXPECT_EQ(loop.worker().id(), runtime.worker().id());
    EXPECT_EQ(runtime.loop().worker().id(), loop.worker().id());
    ASSERT_TRUE(runtime.start());
    ASSERT_TRUE(runtime.wait_until_ready());
    auto root = loop.start(sleep_then_finish(loop.worker(), root_finished));
    runtime.request_stop();
    runtime.join();
    EXPECT_TRUE(admission_ran);
    EXPECT_TRUE(admitted_after_root);
    EXPECT_EQ(std::move(root).get(), 11);
    EXPECT_FALSE(loop.valid());
    EXPECT_EQ(runtime.take_failure(), nullptr);
}

TEST(Ruvia, WorkerRuntimeLoopReportsFailuresToItsRuntime) {
    ruvia::worker_runtime runtime;
    runtime.configure({.startup_ = [&] { runtime.mark_ready(); }});
    const auto loop = runtime.loop();
    ASSERT_TRUE(runtime.start());
    ASSERT_TRUE(runtime.wait_until_ready());
    loop.report_failure(std::make_exception_ptr(std::runtime_error("loop failure")));
    runtime.join();
    EXPECT_NE(runtime.take_failure(), nullptr);
    // A stopped runtime refuses new roots through its loop.
    auto refused = loop.try_start([]() -> ruvia::task<void> { co_return; }());
    EXPECT_EQ(refused.status(), ruvia::post_status::worker_stopping);
    EXPECT_NE(refused.rejected(), nullptr);
}

TEST(Ruvia, WorkerRefPostPayloadReturnsRefusedPayload) {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 1});
    const auto worker_value = loops.loop(0).worker();
    std::promise<int> ran;
    auto accepted = worker_value.post_payload(std::make_unique<int>(3),
        [&ran](std::unique_ptr<int>&& value) -> ruvia::move_only_function<void()> {
            return [value = std::move(value), &ran] { ran.set_value(*value); };
        });
    EXPECT_TRUE(accepted.accepted());
    EXPECT_EQ(accepted.rejected(), nullptr);
    bool wrapped_refused = false;
    auto refused = worker_value.post_payload(std::make_unique<int>(4),
        [&wrapped_refused](std::unique_ptr<int>&&) -> ruvia::move_only_function<void()> {
            wrapped_refused = true;
            return [] {};
        });
    EXPECT_EQ(refused.status(), ruvia::post_status::queue_full);
    EXPECT_FALSE(wrapped_refused);
    ASSERT_NE(refused.rejected(), nullptr);
    EXPECT_EQ(**refused.rejected(), 4);
    auto unbound = ruvia::worker_ref{}.post_payload(std::make_unique<int>(5),
        [](std::unique_ptr<int>&&) -> ruvia::move_only_function<void()> { return [] {}; });
    EXPECT_EQ(unbound.status(), ruvia::post_status::worker_stopping);
    EXPECT_EQ(*std::move(unbound).take_rejected(), 5);
    loops.start();
    auto future = ran.get_future();
    ASSERT_EQ(future.wait_for(std::chrono::seconds(10)), std::future_status::ready);
    EXPECT_EQ(future.get(), 3);
    loops.stop();
    loops.join();
}

}  // namespace worker_runtime_test

namespace ruvia::test::task_scope_tests {

ruvia::task<void> increment(ruvia::worker_ref worker_value, int& value) {
    static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1)));
    ++value;
    co_return;
}

ruvia::task<void> fail() {
    throw std::runtime_error("child failed");
    co_return;
}

ruvia::task<void> no_op() {
    co_return;
}

ruvia::task<void> observe_join(ruvia::task_scope::join_operation join, bool& completed) {
    co_await std::move(join);
    completed = true;
}

ruvia::task<void> join_and_destroy_on_failure(std::optional<ruvia::task_scope>& primary,
    bool& completed_after_node_release) {
    bool caught = false;
    try {
        co_await primary->join();
    } catch (const std::runtime_error& error) {
        caught = std::string_view(error.what()) == "child failed";
    }
    completed_after_node_release = caught && primary->size() == 0;
    primary.reset();
}

ruvia::task<void> verify_stop_callback_reentrant_join_waits_for_finish(
    ruvia::worker_ref worker_value, bool& success) {
    std::optional<ruvia::task_scope> primary(std::in_place, worker_value);
    ruvia::task_scope observer(worker_value);
    bool active_during_stop_callback = false;
    bool completed_after_node_release = false;
    ruvia::stop_registration registration;
    primary->token().register_callback(registration, [&] {
        active_during_stop_callback = primary.has_value() && primary->size() == 1;
        observer.spawn(join_and_destroy_on_failure(
            primary, completed_after_node_release));
    });
    primary->spawn(fail());
    while (primary.has_value()) {
        static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1)));
    }
    co_await observer.join();
    success = active_during_stop_callback && completed_after_node_release &&
              !primary.has_value();
}

ruvia::task<void> wait_for_stop(ruvia::worker_ref worker_value, ruvia::stop_token token, bool& stopped) {
    stopped = co_await ruvia::sleep_for(worker_value, std::chrono::hours(1), std::move(token)) == ruvia::timer_sleep_result::stop_requested;
}

ruvia::task<void> exercise(ruvia::worker_ref worker_value, bool& success) {
    {
        ruvia::task_scope empty_scope(worker_value);
        if (!empty_scope.empty()) {
            co_return;
        }
        {
            auto discarded_cold_join = empty_scope.join();
            static_cast<void>(discarded_cold_join);
        }
        co_await empty_scope.join();
        bool second_join_rejected = false;
        try {
            co_await empty_scope.join();
        } catch (const std::logic_error&) {
            second_join_rejected = true;
        }
        bool spawn_after_join_rejected = false;
        try {
            empty_scope.spawn(no_op());
        } catch (const std::logic_error&) {
            spawn_after_join_rejected = true;
        }
        if (!second_join_rejected || !spawn_after_join_rejected) {
            co_return;
        }
    }

    {
        ruvia::task_scope empty_task_scope(worker_value);
        auto moved_from = no_op();
        auto retained = std::move(moved_from);
        static_cast<void>(retained);
        bool empty_task_rejected = false;
        try {
            empty_task_scope.spawn(std::move(moved_from));
        } catch (const std::logic_error&) {
            empty_task_rejected = true;
        }
        co_await empty_task_scope.join();
        if (!empty_task_rejected || empty_task_scope.size() != 0) {
            co_return;
        }
    }

    {
        int completed = 0;
        ruvia::task_scope cold_join_scope(worker_value);
        cold_join_scope.spawn(increment(worker_value, completed));
        auto first_join = cold_join_scope.join();
        std::optional<ruvia::task_scope::join_operation> second_join;
        bool spawn_before_join_started = false;
        try {
            second_join.emplace(cold_join_scope.join());
        } catch (const std::logic_error&) {
        }
        try {
            cold_join_scope.spawn(increment(worker_value, completed));
            spawn_before_join_started = true;
        } catch (const std::logic_error&) {
        }
        co_await std::move(first_join);
        bool repeated_start_rejected = false;
        if (second_join) {
            try {
                co_await std::move(*second_join);
            } catch (const std::logic_error&) {
                repeated_start_rejected = true;
            }
        }
        if (!spawn_before_join_started || !repeated_start_rejected || completed != 2) {
            co_return;
        }
    }

    {
        int completed = 0;
        ruvia::task_scope running_scope(worker_value);
        auto first_join = running_scope.join();
        auto competing_join = running_scope.join();
        running_scope.spawn(increment(worker_value, completed));
        bool join_completed = false;
        ruvia::task_scope observer(worker_value);
        observer.spawn(observe_join(std::move(first_join), join_completed));
        bool competing_join_rejected = false;
        try {
            co_await std::move(competing_join);
        } catch (const std::logic_error&) {
            competing_join_rejected = true;
        }
        bool spawn_during_join_rejected = false;
        try {
            running_scope.spawn(no_op());
        } catch (const std::logic_error&) {
            spawn_during_join_rejected = true;
        }
        co_await observer.join();
        if (!join_completed || !competing_join_rejected || !spawn_during_join_rejected || completed != 1) {
            co_return;
        }
    }

    {
        ruvia::stop_token retained;
        int notifications = 0;
        {
            ruvia::task_scope scope(worker_value);
            const auto& observed = scope;
            retained = observed.token();
            ruvia::stop_registration registration;
            retained.register_callback(registration, [&] { ++notifications; });
            scope.request_stop();
            scope.request_stop();
            if (observed.token() != retained || !observed.stop_requested() ||
                !retained.stop_requested() || notifications != 1) {
                co_return;
            }
        }
        if (!retained.stop_requested() || notifications != 1) {
            co_return;
        }
    }

    {
        ruvia::task_scope cancelled_scope(worker_value);
        {
            auto discarded_join = cancelled_scope.join();
            static_cast<void>(discarded_join);
        }
        bool stopped = false;
        cancelled_scope.spawn(wait_for_stop(worker_value, cancelled_scope.token(), stopped));
        auto join = cancelled_scope.join();
        cancelled_scope.request_stop();
        co_await std::move(join);
        if (!stopped || !cancelled_scope.stop_requested() || !cancelled_scope.empty() || cancelled_scope.size() != 0) {
            co_return;
        }
    }

    {
        // A scope whose children all completed may be destroyed without a
        // join; an unobserved failure is reported instead of lost.
        int completed = 0;
        {
            ruvia::task_scope unjoined_scope(worker_value);
            unjoined_scope.spawn(increment(worker_value, completed));
            unjoined_scope.spawn(fail());
            // Completion is delivered after the timer pass that resumed the
            // child, so wait until every child has been retired.
            while (unjoined_scope.size() != 0) {
                static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1)));
            }
            if (!unjoined_scope.stop_requested()) {
                co_return;
            }
        }
        if (completed != 1) {
            co_return;
        }
    }
    {
        bool scope_clean_on_retire = true;
        struct unjoined_scope_holder final {
            explicit unjoined_scope_holder(const ruvia::worker_ref& w, bool& clean)
                : clean_(clean),
                  scope_(w) {}
            ~unjoined_scope_holder() {
                if (!scope_.empty()) {
                    clean_ = false;
                }
            }
            bool& clean_;
            ruvia::task_scope scope_;
        };
        auto holder = std::make_shared<unjoined_scope_holder>(worker_value, scope_clean_on_retire);
        auto child_with_holder = [worker_value](std::shared_ptr<unjoined_scope_holder> owned, int delay_ms) -> ruvia::task<void> {
            if (delay_ms > 0) {
                static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(delay_ms)));
            }
            static_cast<void>(owned);
            co_return;
        };
        for (int index = 0; index < 16; ++index) {
            holder->scope_.spawn(child_with_holder(holder, index % 3));
        }
        std::weak_ptr<unjoined_scope_holder> weak_holder = holder;
        holder.reset();
        while (!weak_holder.expired()) {
            static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1)));
        }
        if (!scope_clean_on_retire) {
            co_return;
        }
    }

    {
        ruvia::task_scope completed_failure_scope(worker_value);
        completed_failure_scope.spawn(fail());
        static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1)));
        bool completed_failure_observed = false;
        try {
            co_await completed_failure_scope.join();
        } catch (const std::runtime_error& error) {
            completed_failure_observed = std::string_view(error.what()) == "child failed";
        }
        if (!completed_failure_observed) {
            co_return;
        }
    }

    int calls = 0;
    ruvia::task_scope scope(worker_value);
    scope.spawn(increment(worker_value, calls));
    scope.spawn(fail());
    try {
        co_await scope.join();
    } catch (const std::runtime_error& error) {
        success = calls == 1 && scope.size() == 0 && scope.stop_requested() &&
                  std::string_view(error.what()) == "child failed";
    }
}

#ifndef _WIN32
ruvia::task<void> fail_distinct() {
    throw std::logic_error("second child failed");
    co_return;
}

ruvia::task<void> join_distinct_failures(ruvia::worker_ref worker_value, bool& first_rethrown) {
    ruvia::task_scope scope(worker_value);
    scope.spawn(fail());
    scope.spawn(fail());
    scope.spawn(fail_distinct());
    try {
        co_await scope.join();
    } catch (const std::runtime_error& error) {
        first_rethrown = std::string_view(error.what()) == "child failed";
    }
}

// Redirects stderr, where the unhandled-failure diagnostic writes, into a
// temporary file for the guard's lifetime.
class stderr_capture final {
public:
    stderr_capture()
        : sink_(std::tmpfile()),
          saved_(::dup(STDERR_FILENO)) {
        std::fflush(stderr);
        if (sink_ != nullptr && saved_ >= 0) {
            redirected_ = ::dup2(::fileno(sink_), STDERR_FILENO) >= 0;
        }
    }
    stderr_capture(const stderr_capture&) = delete;
    stderr_capture& operator=(const stderr_capture&) = delete;
    ~stderr_capture() {
        restore();
        if (sink_ != nullptr) {
            std::fclose(sink_);
        }
    }

    // Restores stderr and returns everything written while redirected.
    [[nodiscard]] std::string finish() {
        restore();
        std::string text;
        if (sink_ == nullptr) {
            return text;
        }
        std::rewind(sink_);
        char chunk[256];
        std::size_t read_bytes = 0;
        while ((read_bytes = std::fread(chunk, 1, sizeof(chunk), sink_)) != 0) {
            text.append(chunk, read_bytes);
        }
        return text;
    }

private:
    void restore() noexcept {
        std::fflush(stderr);
        if (redirected_) {
            static_cast<void>(::dup2(saved_, STDERR_FILENO));
            redirected_ = false;
        }
        if (saved_ >= 0) {
            ::close(saved_);
            saved_ = -1;
        }
    }

    std::FILE* sink_;
    int saved_;
    bool redirected_{false};
};

// join rethrows the first child failure; a later, distinct one must still reach
// the unhandled-failure diagnostic, and one equivalent to the first must not
// be reported again.
bool verify_additional_child_failures_are_reported() {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
    bool first_rethrown = false;
    stderr_capture capture;
    ruvia::test::run_worker_tasks(attachment, join_distinct_failures(attachment.loop().worker(), first_rethrown));
    const auto text = capture.finish();
    return first_rethrown &&
           text.find("ruvia: additional task scope child failed: second child failed") != std::string_view::npos &&
           text.find("additional task scope child failed: child failed") == std::string_view::npos;
}
#endif

}  // namespace ruvia::test::task_scope_tests

TEST(Ruvia, TaskScopeLifecycle) {
    ruvia::stop_token retained_token;
    {
        ruvia::stop_source source;
        retained_token = source.token();
        source.request_stop();
    }
    EXPECT_TRUE(retained_token.stop_requested());

    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().worker();
    bool success = false;
    bool reentrant_stop_join_safe = false;
    ruvia::test::run_worker_tasks(attachment,
        ruvia::test::task_scope_tests::exercise(worker_value, success),
        ruvia::test::task_scope_tests::verify_stop_callback_reentrant_join_waits_for_finish(
            worker_value, reentrant_stop_join_safe));
    bool additional_failures_reported = true;
#ifndef _WIN32
    additional_failures_reported = ruvia::test::task_scope_tests::verify_additional_child_failures_are_reported();
#endif
    EXPECT_TRUE(success);
    EXPECT_TRUE(reentrant_stop_join_safe);
    EXPECT_TRUE(additional_failures_reported);
}
