#include <atomic>
#include <barrier>
#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/channel.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/one_shot.h"
#include "ruvia/core/pool_lease_scheduler.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/core/worker_runtime_context.h"

#include "worker_task_fixture.h"

namespace {

bool timer_immediate_shutdown_works() {
    asio::io_context io_context;
    for (int attempt_value = 0; attempt_value < 32; ++attempt_value) {
        io_context.restart();
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 2});
        const auto worker_value = attachment.loop().handle();
        ruvia::worker_timer_registration registration;
        std::promise<void> stopped;
        auto stopped_ready = stopped.get_future();
        asio::post(io_context, [&] {
            (worker_value).schedule_timer(registration, std::chrono::steady_clock::now() + std::chrono::hours(1), [](ruvia::worker_timer_outcome) {});
            attachment.stop();
            asio::post(io_context, [&] {
                stopped.set_value();
            });
        });
        std::thread worker_thread([&] { attachment.run(); });
        stopped_ready.get();
        worker_thread.join();
    }
    return true;
}

bool off_worker_cancellation_can_race_with_timer_shutdown() {
    for (int attempt_value = 0; attempt_value < 64; ++attempt_value) {
        asio::io_context io_context;
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
        const auto worker_value = attachment.loop().handle();
        ruvia::worker_timer_registration registration;
        ruvia::worker_timer_cancellation cancellation;
        std::barrier start(2);
        std::atomic_int completions{0};

        asio::post(io_context, [&] {
            (worker_value).schedule_timer(registration, std::chrono::steady_clock::now() + std::chrono::hours(1), [&](ruvia::worker_timer_outcome outcome) {
                if (outcome == ruvia::worker_timer_outcome::cancelled) {
                    completions.fetch_add(1, std::memory_order_relaxed);
                }
            });
            cancellation = registration.cancellation();
            start.arrive_and_wait();
            attachment.stop();
        });

        std::thread worker_thread([&] { attachment.run(); });
        std::thread cancelling_thread([&] {
            start.arrive_and_wait();
            for (int call = 0; call < 64; ++call) {
                cancellation.cancel();
            }
        });
        cancelling_thread.join();
        worker_thread.join();
        if (completions.load(std::memory_order_relaxed) != 1) {
            return false;
        }
    }
    return true;
}

bool off_worker_cancellation_after_context_stop_does_not_expire_later() {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 2});
    const auto worker_value = attachment.loop().handle();
    auto registration = std::make_unique<ruvia::worker_timer_registration>();
    bool expired = false;
    bool cancelled = false;

    asio::post(io_context, [&] {
        (worker_value).schedule_timer(*registration, std::chrono::steady_clock::now() + std::chrono::milliseconds(1), [&](ruvia::worker_timer_outcome outcome) {
            expired = outcome == ruvia::worker_timer_outcome::expired;
            cancelled = outcome == ruvia::worker_timer_outcome::cancelled;
        });
        io_context.stop();
    });
    attachment.run();

    // Destroying a registration outside the stopped context quietly cancels
    // its callback, including when the caller later restarts the context.
    registration.reset();

    io_context.restart();
    io_context.run_for(std::chrono::milliseconds(20));
    attachment.stop();
    return !expired && !cancelled;
}

bool explicit_timer_cancellation_notifies_after_quiet_destruction() {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 2});
    const auto worker_value = attachment.loop().handle();
    auto registration = std::make_unique<ruvia::worker_timer_registration>();
    bool cancelled = false;

    asio::post(io_context, [&] {
        (worker_value).schedule_timer(*registration, std::chrono::steady_clock::now() + std::chrono::hours(1), [&cancelled](ruvia::worker_timer_outcome outcome) {
            cancelled = outcome == ruvia::worker_timer_outcome::cancelled;
        });
        registration->cancel();
        registration.reset();
        io_context.stop();
    });
    attachment.run();

    io_context.restart();
    while (io_context.poll() != 0) {
    }
    attachment.stop();
    return cancelled;
}

ruvia::task<void> mark_after_sleep(
    ruvia::worker_handle worker_value, bool& completed, bool& reported_elapsed) {
    reported_elapsed = co_await ruvia::sleep_for(worker_value, std::chrono::hours(1)) ==
                       ruvia::timer_sleep_result::elapsed;
    completed = true;
}

ruvia::task<void> exercise(ruvia::event_loop_attachment& attachment,
    ruvia::worker_handle worker_value, bool& success) {
    const bool first_sleep_elapsed = co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1)) ==
                                     ruvia::timer_sleep_result::elapsed;

    bool expired = false;
    bool cancelled = false;
    ruvia::worker_timer_registration expired_timer;
    (worker_value).schedule_timer(expired_timer, std::chrono::steady_clock::now(), [&expired](ruvia::worker_timer_outcome outcome) {
        expired = outcome == ruvia::worker_timer_outcome::expired;
    });
    ruvia::worker_timer_registration cancelled_timer;
    (worker_value).schedule_timer(cancelled_timer, std::chrono::steady_clock::now() + std::chrono::hours(1), [&cancelled](ruvia::worker_timer_outcome outcome) {
        cancelled = outcome == ruvia::worker_timer_outcome::cancelled;
    });
    if (!expired_timer.registered() || !cancelled_timer.registered()) {
        co_return;
    }
    cancelled_timer.cancel();
    static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1)));

    bool cancelled_sleep_resumed = false;
    bool cancelled_sleep_reported_elapsed = true;
    ruvia::task_scope scope(worker_value);
    scope.spawn(mark_after_sleep(worker_value, cancelled_sleep_resumed, cancelled_sleep_reported_elapsed));
    attachment.stop();
    co_await scope.join();
    // A normal sleep reports elapsed; a shutdown-cancelled sleep resumes but
    // reports not-elapsed so a periodic loop can stop instead of re-sleeping.
    success = expired && cancelled && first_sleep_elapsed && cancelled_sleep_resumed &&
              !cancelled_sleep_reported_elapsed;
}

}  // namespace

// A framework-provided wait that ignores the caller's stop token is a hole in
// every deadline built on that token. These pin that sleep_for's stoppable
// overload closes it, and that stopping it does not depend on the timer having
// been long enough to notice.
ruvia::task<void> exercise_stoppable_sleep(const ruvia::worker_handle& worker_value, bool& success) {
    success = false;

    // Not stopped: the sleep runs to completion and reports elapsed.
    ruvia::stop_source idle;
    if (co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1), idle.token()) !=
        ruvia::timer_sleep_result::elapsed) {
        co_return;
    }

    // Already stopped before the call: must not suspend for the full duration.
    ruvia::stop_source stopped;
    stopped.request_stop();
    const auto before = std::chrono::steady_clock::now();
    if (co_await ruvia::sleep_for(worker_value, std::chrono::seconds(30), stopped.token()) !=
        ruvia::timer_sleep_result::stop_requested) {
        co_return;
    }
    if (std::chrono::steady_clock::now() - before > std::chrono::seconds(5)) {
        co_return;
    }

    // Stopped while suspended: the deferred cancel has to cut a sleep that is
    // already parked on the timer queue, which is the case the whole overload
    // exists for.
    ruvia::stop_source inflight;
    static_cast<void>(worker_value.post([&inflight] { inflight.request_stop(); }));
    const auto parked = std::chrono::steady_clock::now();
    if (co_await ruvia::sleep_for(worker_value, std::chrono::seconds(30), inflight.token()) !=
        ruvia::timer_sleep_result::stop_requested) {
        co_return;
    }
    if (std::chrono::steady_clock::now() - parked > std::chrono::seconds(5)) {
        co_return;
    }

    // Cancellation from another thread must resume the sleeping task with the
    // public stop_requested outcome.
    ruvia::stop_source cross_thread;
    std::thread stopper([&cross_thread] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        cross_thread.request_stop();
    });
    const auto cross_thread_result =
        co_await ruvia::sleep_for(worker_value, std::chrono::seconds(30), cross_thread.token());
    stopper.join();
    if (cross_thread_result != ruvia::timer_sleep_result::stop_requested) {
        co_return;
    }

    // A stop that arrives long after the sleep already finished must be a
    // no-op, not a use-after-free of the awaiter's timer registration.
    ruvia::stop_source late;
    if (co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1), late.token()) !=
        ruvia::timer_sleep_result::elapsed) {
        co_return;
    }
    late.request_stop();
    static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(5)));

    success = true;
}

bool stoppable_sleep_works() {
    // Keep token cancellation independent of the shutdown cases above.
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().handle();
    bool success = false;
    asio::co_spawn(io_context,
        ruvia::as_awaitable(exercise_stoppable_sleep(worker_value, success)),
        [&attachment](std::exception_ptr failure) {
            attachment.stop();
            if (failure) {
                std::rethrow_exception(failure);
            }
        });
    attachment.run();
    attachment.stop();
    return success;
}

ruvia::task<void> receive_through_timer_stop(
    ruvia::channel_receiver<int>& receiver, ruvia::worker_wait_status& status) {
    const auto result_value = co_await receiver.receive_for(std::chrono::hours(1));
    status = result_value.status();
}

ruvia::task<void> acquire_after_timer_stop(
    ruvia::pool_lease_scheduler& pool, std::optional<ruvia::pool_waiter_result::status_type>& status) {
    const auto result_value = co_await pool.acquire(std::chrono::milliseconds(1));
    status = result_value.status();
}

// Once the worker's timer queue stops, every timed primitive reports its
// shutdown result instead of throwing, whether it was pending or starts later.
ruvia::task<void> exercise_after_timer_stop(ruvia::worker_runtime_context& runtime, bool& success) {
    success = false;
    const auto& worker_value = runtime.handle();
    auto [sender, receiver] = ruvia::make_channel<int>(worker_value, {.capacity_ = 1});
    auto [completion, one_shot_receiver] = ruvia::make_one_shot<int>(worker_value);

    auto pending_status = ruvia::worker_wait_status::value;
    {
        ruvia::task_scope scope(worker_value);
        scope.spawn(receive_through_timer_stop(receiver, pending_status));
        // Let the receive park on its deadline before the queue stops.
        static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1)));
        runtime.stop_timers();
        co_await scope.join();
    }
    if (pending_status != ruvia::worker_wait_status::worker_stopping) {
        co_return;
    }

    if (co_await ruvia::sleep_for(worker_value, std::chrono::hours(1)) !=
        ruvia::timer_sleep_result::stop_requested) {
        co_return;
    }
    ruvia::stop_source idle;
    if (co_await ruvia::sleep_for(worker_value, std::chrono::hours(1), idle.token()) !=
        ruvia::timer_sleep_result::stop_requested) {
        co_return;
    }
    if ((co_await receiver.receive_for(std::chrono::hours(1))).status() !=
        ruvia::worker_wait_status::worker_stopping) {
        co_return;
    }
    if ((co_await one_shot_receiver.wait_for(std::chrono::hours(1))).status() !=
        ruvia::worker_wait_status::worker_stopping) {
        co_return;
    }

    ruvia::worker_timer_registration registration;
    bool invoked = false;
    const auto schedule_status = worker_value.schedule_timer(registration,
        std::chrono::steady_clock::now() + std::chrono::hours(1),
        [&invoked](ruvia::worker_timer_outcome) { invoked = true; });
    if (schedule_status != ruvia::worker_timer_schedule_status::worker_stopping || registration.registered() ||
        invoked) {
        co_return;
    }

    // A timed acquire keeps waiting without a timer and ends with the pool.
    ruvia::pool_lease_scheduler pool(1, worker_value);
    const auto held = co_await pool.acquire(std::nullopt);
    if (!held.acquired()) {
        co_return;
    }
    std::optional<ruvia::pool_waiter_result::status_type> waiting_status;
    {
        ruvia::task_scope scope(worker_value);
        scope.spawn(acquire_after_timer_stop(pool, waiting_status));
        static_cast<void>(pool.close());
        co_await scope.join();
    }
    success = waiting_status == ruvia::pool_waiter_result::status_type::closed;
}

bool timed_primitives_report_timer_stop() {
    asio::io_context io_context;
    ruvia::worker_runtime_context runtime(io_context, 16);
    bool success = false;
    ruvia::test::run_worker_tasks(runtime, exercise_after_timer_stop(runtime, success));
    runtime.detach();
    return success;
}

// An expired registration is released by its worker and may be rescheduled on
// another; a still-pending one is rejected until it is cancelled.
bool expired_registration_reschedules_on_another_worker() {
    asio::io_context first_context;
    asio::io_context second_context;
    ruvia::worker_runtime_context first(first_context, 8);
    ruvia::worker_runtime_context second(second_context, 8);
    ruvia::worker_timer_registration expiring;
    ruvia::worker_timer_registration pending;
    bool first_expired = false;
    bool second_expired = false;
    bool pending_rejected = false;
    bool cancelled_rescheduled = false;
    const auto drive = [](asio::io_context& context) {
        context.restart();
        context.run_for(std::chrono::milliseconds(20));
    };

    const auto first_post = first.handle().post([&] {
        static_cast<void>(first.handle().schedule_timer(expiring, std::chrono::steady_clock::now(),
            [&first_expired](ruvia::worker_timer_outcome outcome) {
                first_expired = outcome == ruvia::worker_timer_outcome::expired;
            }));
        static_cast<void>(first.handle().schedule_timer(pending,
            std::chrono::steady_clock::now() + std::chrono::hours(1), [](ruvia::worker_timer_outcome) {}));
    });
    if (!first_post.accepted()) {
        return false;
    }
    drive(first_context);

    const auto second_post = second.handle().post([&] {
        try {
            static_cast<void>(second.handle().schedule_timer(pending,
                std::chrono::steady_clock::now(), [](ruvia::worker_timer_outcome) {}));
        } catch (const std::logic_error&) {
            pending_rejected = true;
        }
        static_cast<void>(second.handle().schedule_timer(expiring, std::chrono::steady_clock::now(),
            [&second_expired](ruvia::worker_timer_outcome outcome) {
                second_expired = outcome == ruvia::worker_timer_outcome::expired;
            }));
    });
    if (!second_post.accepted()) {
        return false;
    }
    drive(second_context);

    // Cancellation from this thread is applied on the first worker.
    pending.cancel_quietly();
    drive(first_context);
    const auto rescheduled_post = second.handle().post([&] {
        cancelled_rescheduled = second.handle().schedule_timer(pending,
                                    std::chrono::steady_clock::now() + std::chrono::hours(1),
                                    [](ruvia::worker_timer_outcome) {}) ==
                                ruvia::worker_timer_schedule_status::scheduled;
        pending.cancel_quietly();
    });
    if (!rescheduled_post.accepted()) {
        return false;
    }
    drive(second_context);
    first.detach();
    second.detach();
    return first_expired && second_expired && pending_rejected && cancelled_rescheduled;
}

int main() {
    if (!timer_immediate_shutdown_works() ||
        !off_worker_cancellation_can_race_with_timer_shutdown() ||
        !off_worker_cancellation_after_context_stop_does_not_expire_later() ||
        !explicit_timer_cancellation_notifies_after_quiet_destruction() || !stoppable_sleep_works() ||
        !timed_primitives_report_timer_stop() || !expired_registration_reschedules_on_another_worker()) {
        return 1;
    }
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().handle();
    bool success = false;
    asio::co_spawn(io_context, ruvia::as_awaitable(exercise(attachment, worker_value, success)),
        asio::detached);
    attachment.run();
    attachment.stop();
    return success ? 0 : 1;
}
