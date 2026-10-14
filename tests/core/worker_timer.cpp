#include "ruvia/core/worker_timer.h"

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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

bool timer_duration_conversions_preserve_integer_ticks_and_saturate() {
    using namespace std::chrono;
    for (const nanoseconds value : {nanoseconds(9'000'000'000'000'000'001LL),
             nanoseconds(-9'000'000'000'000'000'001LL), nanoseconds(1), nanoseconds(-1)}) {
        if (ruvia::worker_timer_saturating_duration_cast(value) != duration_cast<steady_clock::duration>(value)) {
            return false;
        }
    }
    const milliseconds coarse(9'000'000'000'001LL);
    using picoseconds_type = duration<std::int64_t, std::pico>;
    const picoseconds_type fine(9'223'000'000'000'001'000LL);
    if (ruvia::worker_timer_saturating_duration_cast(coarse) != duration_cast<steady_clock::duration>(coarse) ||
        ruvia::worker_timer_saturating_duration_cast(fine) != duration_cast<steady_clock::duration>(fine) ||
        ruvia::worker_timer_saturating_duration_cast(seconds::max()) != steady_clock::duration::max() ||
        ruvia::worker_timer_saturating_duration_cast(seconds::min()) != steady_clock::duration::min() ||
        ruvia::worker_timer_saturating_duration_cast(duration<std::uint64_t, std::nano>((std::numeric_limits<std::uint64_t>::max)())) != steady_clock::duration::max()) {
        return false;
    }
    return ruvia::worker_timer_ceil_milliseconds(duration_cast<steady_clock::duration>(nanoseconds(9'000'000'000'000'000'001LL))) == milliseconds(9'000'000'000'001LL) &&
           ruvia::worker_timer_ceil_milliseconds(duration_cast<steady_clock::duration>(milliseconds(1))) == milliseconds(1) &&
           ruvia::worker_timer_ceil_milliseconds(duration_cast<steady_clock::duration>(nanoseconds(1))) == milliseconds(1) &&
           ruvia::worker_timer_ceil_milliseconds(steady_clock::duration::min()) == milliseconds::zero();
}

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

bool off_worker_cancellation_before_queued_stop_timers_does_not_invoke_callback() {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 4});
    const auto worker_value = attachment.loop().handle();
    auto registration = std::make_unique<ruvia::worker_timer_registration>();
    bool invoked = false;

    asio::post(io_context, [&] {
        (worker_value).schedule_timer(*registration, std::chrono::steady_clock::now() + std::chrono::hours(1), [&invoked](ruvia::worker_timer_outcome) { invoked = true; });
        io_context.stop();
    });
    attachment.run();

    attachment.stop();
    registration.reset();

    io_context.restart();
    attachment.run();
    return !invoked;
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
        registration->cancel_and_notify();
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
    bool quiet_invoked = false;
    ruvia::worker_timer_registration quiet_timer;
    (worker_value).schedule_timer(quiet_timer, std::chrono::steady_clock::now() + std::chrono::hours(1), [&quiet_invoked](ruvia::worker_timer_outcome) {
        quiet_invoked = true;
    });
    if (!expired_timer.registered() || !cancelled_timer.registered() || !quiet_timer.registered()) {
        co_return;
    }
    cancelled_timer.cancel_and_notify();
    // The owner-side cancel() never delivers the completion.
    quiet_timer.cancel();
    static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1)));

    bool cancelled_sleep_resumed = false;
    bool cancelled_sleep_reported_elapsed = true;
    ruvia::task_scope scope(worker_value);
    scope.spawn(mark_after_sleep(worker_value, cancelled_sleep_resumed, cancelled_sleep_reported_elapsed));
    attachment.stop();
    co_await scope.join();
    // A normal sleep reports elapsed; a shutdown-cancelled sleep resumes but
    // reports not-elapsed so a periodic loop can stop instead of re-sleeping.
    success = expired && cancelled && !quiet_invoked && first_sleep_elapsed && cancelled_sleep_resumed &&
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

    // An unbounded delay saturates instead of overflowing into a negative
    // duration that would report elapsed without ever suspending.
    ruvia::stop_source unbounded;
    static_cast<void>(worker_value.post([&unbounded] { unbounded.request_stop(); }));
    if (co_await ruvia::sleep_for(worker_value, std::chrono::hours::max(), unbounded.token()) !=
        ruvia::timer_sleep_result::stop_requested) {
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
    ruvia::pool_lease_scheduler& pool, std::optional<ruvia::worker_wait_result<std::size_t>>& result) {
    result.emplace(co_await pool.acquire(std::chrono::milliseconds(1)));
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
    if (!held.has_value()) {
        co_return;
    }
    std::optional<ruvia::worker_wait_result<std::size_t>> waiting_result;
    {
        ruvia::task_scope scope(worker_value);
        scope.spawn(acquire_after_timer_stop(pool, waiting_result));
        static_cast<void>(pool.close());
        co_await scope.join();
    }
    success = waiting_result.has_value() && waiting_result->status() == ruvia::worker_wait_status::closed;
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
    pending.cancel();
    drive(first_context);
    const auto rescheduled_post = second.handle().post([&] {
        cancelled_rescheduled = second.handle().schedule_timer(pending,
                                    std::chrono::steady_clock::now() + std::chrono::hours(1),
                                    [](ruvia::worker_timer_outcome) {}) ==
                                ruvia::worker_timer_schedule_status::scheduled;
        pending.cancel();
    });
    if (!rescheduled_post.accepted()) {
        return false;
    }
    drive(second_context);
    first.detach();
    second.detach();
    return first_expired && second_expired && pending_rejected && cancelled_rescheduled;
}

bool timer_callback_failure_preserves_remaining_operations() {
    asio::io_context io_context;
    ruvia::worker_runtime_context runtime(io_context, 8);
    const auto& worker_value = runtime.handle();
    ruvia::worker_timer_registration failing_timer;
    ruvia::worker_timer_registration remaining_timer;
    ruvia::worker_timer_registration cancelled_timer;
    bool remaining_expired = false;
    bool rescheduled_expired = false;
    bool cancellation_observed = false;
    std::weak_ptr<int> failed_capture;
    std::weak_ptr<int> completed_capture;
    std::weak_ptr<int> cancelled_capture;

    asio::post(io_context, [&] {
        const auto deadline = std::chrono::steady_clock::now();
        auto owned = std::make_shared<int>(1);
        failed_capture = owned;
        static_cast<void>(worker_value.schedule_timer(failing_timer, deadline,
            [owned = std::move(owned)](ruvia::worker_timer_outcome) {
                throw std::runtime_error("timer completion failure");
            }));
        owned = std::make_shared<int>(2);
        completed_capture = owned;
        static_cast<void>(worker_value.schedule_timer(remaining_timer, deadline,
            [owned = std::move(owned), &remaining_expired](ruvia::worker_timer_outcome outcome) {
                remaining_expired = outcome == ruvia::worker_timer_outcome::expired;
            }));
        owned = std::make_shared<int>(3);
        cancelled_capture = owned;
        static_cast<void>(worker_value.schedule_timer(cancelled_timer, deadline + std::chrono::hours(1),
            [owned = std::move(owned), &cancellation_observed](ruvia::worker_timer_outcome outcome) {
                cancellation_observed = outcome == ruvia::worker_timer_outcome::cancelled;
            }));
    });

    bool failure_observed = false;
    try {
        io_context.run_for(std::chrono::milliseconds(20));
    } catch (const std::runtime_error& error) {
        failure_observed = std::string_view(error.what()) == "timer completion failure";
    }
    asio::post(io_context, [&] {
        cancelled_timer.cancel_and_notify();
        static_cast<void>(worker_value.schedule_timer(failing_timer, std::chrono::steady_clock::now(),
            [&rescheduled_expired](ruvia::worker_timer_outcome outcome) {
                rescheduled_expired = outcome == ruvia::worker_timer_outcome::expired;
            }));
    });
    io_context.run_for(std::chrono::milliseconds(20));
    const bool success = failure_observed && remaining_expired && rescheduled_expired &&
                         cancellation_observed && failed_capture.expired() && completed_capture.expired() &&
                         cancelled_capture.expired();
    runtime.stop_timers();
    runtime.detach();
    return success;
}

// A completion that re-arms an already-due deadline must not keep one timer
// pass running: other handlers, such as I/O and stop controls, run between
// rounds.
bool self_rescheduling_due_timer_yields_to_other_handlers() {
    constexpr int round_limit = 256;
    asio::io_context io_context;
    ruvia::worker_runtime_context runtime(io_context, 8);
    const auto& worker_value = runtime.handle();
    ruvia::worker_timer_registration timer;
    int rounds = 0;
    int rounds_before_handler = -1;
    std::function<void(ruvia::worker_timer_outcome)> tick;
    tick = [&](ruvia::worker_timer_outcome outcome) {
        if (outcome != ruvia::worker_timer_outcome::expired) {
            return;
        }
        if (++rounds == 1) {
            asio::post(io_context, [&] {
                rounds_before_handler = rounds;
                io_context.stop();
            });
        }
        if (rounds < round_limit && rounds_before_handler < 0) {
            static_cast<void>(worker_value.schedule_timer(timer, std::chrono::steady_clock::now(),
                [&tick](ruvia::worker_timer_outcome next) { tick(next); }));
        }
    };
    asio::post(io_context, [&] {
        static_cast<void>(worker_value.schedule_timer(timer, std::chrono::steady_clock::now(),
            [&tick](ruvia::worker_timer_outcome next) { tick(next); }));
    });
    io_context.run();
    runtime.stop_timers();
    runtime.detach();
    return rounds_before_handler >= 1 && rounds_before_handler < round_limit;
}

bool self_rescheduling_due_timer_preserves_other_expirations() {
    asio::io_context io_context;
    ruvia::worker_runtime_context runtime(io_context, 8);
    const auto worker = runtime.handle();
    ruvia::worker_timer_registration repeating;
    ruvia::worker_timer_registration other;
    const auto earlier = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    std::string order;
    int rounds = 0;
    std::function<void(ruvia::worker_timer_outcome)> tick;
    tick = [&](ruvia::worker_timer_outcome outcome) {
        if (outcome != ruvia::worker_timer_outcome::expired) {
            return;
        }
        order += 'A';
        if (++rounds < 3) {
            static_cast<void>(worker.schedule_timer(repeating, earlier,
                [&](ruvia::worker_timer_outcome next) { tick(next); }));
        }
    };
    asio::post(io_context, [&] {
        static_cast<void>(worker.schedule_timer(repeating, earlier,
            [&](ruvia::worker_timer_outcome next) { tick(next); }));
        static_cast<void>(worker.schedule_timer(other, earlier + std::chrono::seconds(1),
            [&](ruvia::worker_timer_outcome outcome) {
                if (outcome == ruvia::worker_timer_outcome::expired) {
                    order += 'B';
                }
            }));
    });
    io_context.run();
    runtime.detach();
    return order == "ABAA";
}

int main() {
    if (!timer_duration_conversions_preserve_integer_ticks_and_saturate() ||
        !timer_immediate_shutdown_works() ||
        !off_worker_cancellation_can_race_with_timer_shutdown() ||
        !off_worker_cancellation_after_context_stop_does_not_expire_later() ||
        !off_worker_cancellation_before_queued_stop_timers_does_not_invoke_callback() ||
        !explicit_timer_cancellation_notifies_after_quiet_destruction() || !stoppable_sleep_works() ||
        !timed_primitives_report_timer_stop() || !expired_registration_reschedules_on_another_worker() ||
        !timer_callback_failure_preserves_remaining_operations() ||
        !self_rescheduling_due_timer_yields_to_other_handlers() ||
        !self_rescheduling_due_timer_preserves_other_expirations()) {
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
