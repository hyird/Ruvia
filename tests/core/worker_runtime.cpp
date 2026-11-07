#include "ruvia/core/worker_runtime.h"

#include <gtest/gtest.h>

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

#include "test_exceptions.h"

namespace worker_runtime_test {

using state = ruvia::worker_runtime_state;

// Reading failure() marks it observed, so it is not offered on a const runtime.
template <typename runtime_type>
concept failure_readable_through_const = requires(const runtime_type& runtime_value) { runtime_value.failure(); };
static_assert(!failure_readable_through_const<ruvia::worker_runtime>);
static_assert(requires(ruvia::worker_runtime& runtime_value) { runtime_value.failure(); });

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
    const auto worker_value = runtime.handle();
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
    EXPECT_TRUE(runtime.start());
    runtime.join();
    EXPECT_TRUE(failure_on_owner);
    EXPECT_TRUE(shutdown_after_completion);
    EXPECT_EQ(reported, runtime.failure());
    EXPECT_EQ(phases, (std::vector<int>{1, 2, 3, 4}));
    EXPECT_TRUE(runtime.failure() != nullptr);
    EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)runtime.start(); }));
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
    EXPECT_TRUE(!startup_ran);
    EXPECT_TRUE(stop_ran);
    EXPECT_TRUE(finalizer_ran);
    EXPECT_TRUE(shutdown_ran);
}

TEST(Ruvia, worker_runtime_rejects_self_join_without_losing_cleanup) {
    ruvia::worker_runtime runtime;
    bool rejected{};
    runtime.configure({
        .startup_ = [&] { rejected = ruvia::testing::throws_on([&] { runtime.join(); }); },
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
        admission_closed = !runtime.handle().accepting();
        self_join_rejected = ruvia::testing::throws_on([&] { runtime.join(); });
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
    const auto worker_value = runtime.handle();
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
    EXPECT_TRUE(runtime.failure() == nullptr);
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
    EXPECT_EQ(message, std::string("first"));
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
    EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)runtime.wait_until_serving(); }));
    runtime.join();
    EXPECT_TRUE(runtime.failure() != nullptr);
}

TEST(Ruvia, worker_group_coordinates_startup_serving_and_retirement) {
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
            second_retired = group.stop_requested() && second.handle().is_current();
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
        admissions_closed = !producer.handle().accepting() && !consumer.handle().accepting();
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
        .shutdown_ = [&] { shutdown_on_owner = producer.handle().is_current(); },
    });
    group.add(producer);
    group.add(consumer);
    EXPECT_TRUE(producer.handle().post([&] { accepted_work_ran = true; }).accepted());
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
    EXPECT_TRUE(stop_observed);
    EXPECT_TRUE(admissions_closed);
    EXPECT_EQ(message, std::string("external producer failure"));
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
    EXPECT_EQ(message, std::string("member startup failed"));
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
    const bool producer_accepting = producer.handle().accepting();
    const bool consumer_accepting = consumer.handle().accepting();
    const bool consumer_accepted_work = consumer.handle().post([] {}).accepted();
    consumer.finalize();
    const auto failure = group.join();

    EXPECT_TRUE(started);
    EXPECT_TRUE(consumer_served.load());
    EXPECT_EQ(message, std::string("producer serve failed"));
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
        const bool pending_accepting = pending.handle().accepting();
        const auto failure = group.join();

        EXPECT_TRUE(!watchdog_fired.load());
        EXPECT_TRUE(!started);
        EXPECT_EQ(message, std::string(with_failure ? "peer startup failed" : "worker stopped before the group was ready"));
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
    const bool duplicate_rejected = ruvia::testing::throws_on([&] { group.add(member); });
    bool other_group_rejected = false;
    bool stopped_group_rejected = false;
    {
        ruvia::worker_group other;
        other_group_rejected = ruvia::testing::throws_on([&] { other.add(member); });
        other.add(second);
        other.request_stop();
        stopped_group_rejected = ruvia::testing::throws_on([&] { other.add(third); });
        EXPECT_TRUE(other.join() == nullptr);
    }
    const bool started = group.start();
    const bool launched_group_rejected = ruvia::testing::throws_on([&] { group.add(third); });
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
    const bool peer_accepting = peer.handle().post([] {}).accepted();
    member.join();
    peer.join();
    EXPECT_TRUE(members_still_running);
    EXPECT_TRUE(failure_recorded);
    EXPECT_TRUE(peer_accepting);
    EXPECT_EQ(failure_reports.load(), 1);
    EXPECT_EQ(member.failure(), error);
}

TEST(Ruvia, worker_group_failure_hook_can_release_completed_member_observations) {
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
    EXPECT_TRUE(recorded);
    EXPECT_TRUE(stop_observed);
    EXPECT_TRUE(group == nullptr);
    EXPECT_EQ(failure_reports, 1);
    EXPECT_EQ(failing.failure(), error);
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

TEST(Ruvia, event_loop_cleanup_drains_owned_inputs_after_business_admission_closes) {
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
    EXPECT_TRUE(!loop.handle().accepting());
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
    const auto worker_value = loop.handle();
    bool ran{};
    EXPECT_TRUE(loop.defer_cleanup([&] { ran = worker_value.is_current(); }));
    pool.join();
    EXPECT_TRUE(ran);
    EXPECT_TRUE(!loop.handle().valid());
}

TEST(Ruvia, event_loop_draining_post_can_schedule_defer_cleanup_after_stop) {
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    const auto loop = pool.loop(0);
    const auto worker_value = loop.handle();
    bool post_ran = false;
    bool cleanup_scheduled = false;
    bool cleanup_ran = false;
    EXPECT_EQ(loop.handle().post([loop, worker_value, &post_ran, &cleanup_scheduled, &cleanup_ran] {
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
        EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)stopped_runtime.start(); }));
        stopped_runtime.join();
    }
    {
        ruvia::worker_runtime member;
        member.configure({.startup_ = [&] { member.mark_ready(); }});
        ruvia::worker_group group;
        group.add(member);
        EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)group.serve(); }));
        EXPECT_TRUE(group.start());
        EXPECT_TRUE(group.serve());
        EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)group.serve(); }));
        group.request_stop();
        EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)group.start(); }));
        std::promise<std::exception_ptr> other_joined;
        std::thread joiner([&] { other_joined.set_value(group.join()); });
        EXPECT_TRUE(group.join() == nullptr);
        EXPECT_TRUE(other_joined.get_future().get() == nullptr);
        joiner.join();
    }
}

}  // namespace worker_runtime_test
