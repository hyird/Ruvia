#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <ratio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_wait.h"

#include "worker_task_fixture.h"

namespace {

template <typename worker_type, typename... token_types>
concept can_sleep_for = requires(worker_type&& worker, token_types&&... tokens) {
    ruvia::sleep_for(std::forward<worker_type>(worker), std::chrono::milliseconds(1),
        std::forward<token_types>(tokens)...);
};

template <typename worker_type, typename... token_types>
concept can_sleep_until = requires(worker_type&& worker, token_types&&... tokens) {
    ruvia::sleep_until(std::forward<worker_type>(worker), std::chrono::steady_clock::time_point{},
        std::forward<token_types>(tokens)...);
};

static_assert(can_sleep_for<ruvia::worker_ref&>);
static_assert(can_sleep_for<const ruvia::worker_ref&>);
static_assert(!can_sleep_for<ruvia::worker_ref&&>);
static_assert(!can_sleep_for<const ruvia::worker_ref&&>);
static_assert(can_sleep_for<const ruvia::worker_ref&, ruvia::stop_token>);
static_assert(!can_sleep_for<const ruvia::worker_ref&&, ruvia::stop_token>);
static_assert(can_sleep_until<ruvia::worker_ref&>);
static_assert(can_sleep_until<const ruvia::worker_ref&>);
static_assert(!can_sleep_until<ruvia::worker_ref&&>);
static_assert(!can_sleep_until<const ruvia::worker_ref&&>);
static_assert(can_sleep_until<const ruvia::worker_ref&, ruvia::stop_token>);
static_assert(!can_sleep_until<const ruvia::worker_ref&&, ruvia::stop_token>);

bool timer_duration_conversions_preserve_integer_ticks_and_saturate() {
    using namespace std::chrono;
    for (const nanoseconds value : {nanoseconds(9'000'000'000'000'000'001LL),
             nanoseconds(-9'000'000'000'000'000'001LL), nanoseconds(1), nanoseconds(-1)}) {
        if (ruvia::saturating_duration_cast(value) != duration_cast<steady_clock::duration>(value)) {
            return false;
        }
    }
    const milliseconds coarse(9'000'000'000'001LL);
    using picoseconds_type = duration<std::int64_t, std::pico>;
    const picoseconds_type fine(9'223'000'000'000'001'000LL);
    if (ruvia::saturating_duration_cast(coarse) != duration_cast<steady_clock::duration>(coarse) ||
        ruvia::saturating_duration_cast(fine) != duration_cast<steady_clock::duration>(fine) ||
        ruvia::saturating_duration_cast(seconds::max()) != steady_clock::duration::max() ||
        ruvia::saturating_duration_cast(seconds::min()) != steady_clock::duration::min() ||
        ruvia::saturating_duration_cast(duration<std::uint64_t, std::nano>((std::numeric_limits<std::uint64_t>::max)())) != steady_clock::duration::max()) {
        return false;
    }
    return ruvia::ceil_milliseconds(duration_cast<steady_clock::duration>(nanoseconds(9'000'000'000'000'000'001LL))) == milliseconds(9'000'000'000'001LL) &&
           ruvia::ceil_milliseconds(duration_cast<steady_clock::duration>(milliseconds(1))) == milliseconds(1) &&
           ruvia::ceil_milliseconds(duration_cast<steady_clock::duration>(nanoseconds(1))) == milliseconds(1) &&
           ruvia::ceil_milliseconds(steady_clock::duration::min()) == milliseconds::zero();
}

bool timer_immediate_shutdown_works() {
    asio::io_context io_context;
    for (int attempt_value = 0; attempt_value < 32; ++attempt_value) {
        io_context.restart();
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 2});
        const auto worker_value = attachment.loop().worker();
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
        const auto worker_value = attachment.loop().worker();
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
    const auto worker_value = attachment.loop().worker();
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
    const auto worker_value = attachment.loop().worker();
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
    const auto worker_value = attachment.loop().worker();
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

bool on_worker_token_cancellation_followed_by_owner_cancel_withdraws_callback() {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 4});
    const auto worker_value = attachment.loop().worker();
    struct timer_owner final {
        ruvia::worker_timer_registration registration_;
    };
    auto owner = std::make_unique<timer_owner>();
    // The posted cancellation turn runs only after this handler returns, so a
    // callback that runs at all runs after the owner was destroyed.
    bool callback_ran = false;
    auto capture = std::make_shared<int>(1);
    std::weak_ptr<int> weak_capture = capture;

    asio::post(io_context, [&] {
        static_cast<void>(worker_value.schedule_timer(owner->registration_,
            std::chrono::steady_clock::now(),
            [&callback_ran, captured = std::move(capture)](ruvia::worker_timer_outcome) {
                static_cast<void>(captured);
                callback_ran = true;
            }));
        auto cancellation = owner->registration_.cancellation();
        cancellation.cancel();
        owner->registration_.cancel();
        owner.reset();
        attachment.stop();
    });
    attachment.run();
    return !callback_ran && weak_capture.expired();
}

ruvia::task<void> mark_after_sleep(
    ruvia::worker_ref worker_value, bool& completed, bool& reported_elapsed) {
    reported_elapsed = co_await ruvia::sleep_for(worker_value, std::chrono::hours(1)) ==
                       ruvia::timer_sleep_result::elapsed;
    completed = true;
}

ruvia::task<void> exercise(ruvia::event_loop_attachment& attachment,
    ruvia::worker_ref worker_value, bool& success) {
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
ruvia::task<void> exercise_stoppable_sleep(const ruvia::worker_ref& worker_value, bool& success) {
    success = false;

    // Not stopped: the sleep runs to completion and reports elapsed.
    ruvia::stop_source idle;
    if (co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1), idle.token()) !=
        ruvia::timer_sleep_result::elapsed) {
        co_return;
    }
    if (co_await ruvia::sleep_until(worker_value,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(1), idle.token()) !=
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
    const auto worker_value = attachment.loop().worker();
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
    const auto result_value = co_await receiver.receive({.timeout_ = std::chrono::hours(1)});
    status = result_value.status();
}

ruvia::task<void> acquire_after_timer_stop(
    ruvia::pool_lease_scheduler& pool, std::optional<ruvia::worker_wait_result<std::size_t>>& result) {
    result.emplace(co_await pool.acquire({.timeout_ = std::chrono::milliseconds(1)}));
}

// Once the worker stops, every timed primitive reports its shutdown result
// instead of throwing, whether it was pending or starts later.
ruvia::task<void> exercise_after_timer_stop(ruvia::event_loop_attachment& attachment, bool& success) {
    success = false;
    const auto loop = attachment.loop();
    const auto& worker_value = loop.worker();
    auto [sender, receiver] = ruvia::make_channel<int>(worker_value, {.capacity_ = 1});
    auto [completion, one_shot_receiver] = ruvia::make_one_shot<int>(worker_value);

    auto pending_status = ruvia::worker_wait_status::value;
    {
        ruvia::task_scope scope(worker_value);
        scope.spawn(receive_through_timer_stop(receiver, pending_status));
        // Let the receive park on its deadline before the worker stops.
        static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1)));
        attachment.stop();
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
    if ((co_await receiver.receive({.timeout_ = std::chrono::hours(1)})).status() !=
        ruvia::worker_wait_status::worker_stopping) {
        co_return;
    }
    if ((co_await one_shot_receiver.wait({.timeout_ = std::chrono::hours(1)})).status() !=
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

    // A free slot is still leased; a saturated timed acquire reports the
    // shutdown like every other timed primitive.
    ruvia::pool_lease_scheduler pool(1, worker_value);
    const auto held = co_await pool.acquire();
    if (!held.has_value()) {
        co_return;
    }
    std::optional<ruvia::worker_wait_result<std::size_t>> waiting_result;
    {
        ruvia::task_scope scope(worker_value);
        scope.spawn(acquire_after_timer_stop(pool, waiting_result));
        co_await scope.join();
    }
    pool.release(held.value());
    success = waiting_result.has_value() && waiting_result->status() == ruvia::worker_wait_status::worker_stopping;
}

bool timed_primitives_report_timer_stop() {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 16});
    bool success = false;
    ruvia::test::run_worker_tasks(attachment, exercise_after_timer_stop(attachment, success));
    return success;
}

// An expired registration is released by its worker and may be rescheduled on
// another; a still-pending one is rejected until it is cancelled.
bool expired_registration_reschedules_on_another_worker() {
    asio::io_context first_context;
    asio::io_context second_context;
    auto first = ruvia::attach_event_loop(first_context, {.queue_capacity_ = 8});
    auto second = ruvia::attach_event_loop(second_context, {.queue_capacity_ = 8});
    const auto first_worker = first.loop().worker();
    const auto second_worker = second.loop().worker();
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
    const auto retire = [](ruvia::event_loop_attachment& attachment, asio::io_context& context) {
        attachment.stop();
        context.restart();
        attachment.run();
    };

    const auto first_post = first_worker.post([&] {
        static_cast<void>(first_worker.schedule_timer(expiring, std::chrono::steady_clock::now(),
            [&first_expired](ruvia::worker_timer_outcome outcome) {
                first_expired = outcome == ruvia::worker_timer_outcome::expired;
            }));
        static_cast<void>(first_worker.schedule_timer(pending,
            std::chrono::steady_clock::now() + std::chrono::hours(1), [](ruvia::worker_timer_outcome) {}));
    });
    if (!first_post.accepted()) {
        return false;
    }
    drive(first_context);

    const auto second_post = second_worker.post([&] {
        try {
            static_cast<void>(second_worker.schedule_timer(pending,
                std::chrono::steady_clock::now(), [](ruvia::worker_timer_outcome) {}));
        } catch (const std::logic_error&) {
            pending_rejected = true;
        }
        static_cast<void>(second_worker.schedule_timer(expiring, std::chrono::steady_clock::now(),
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
    const auto rescheduled_post = second_worker.post([&] {
        cancelled_rescheduled = second_worker.schedule_timer(pending,
                                    std::chrono::steady_clock::now() + std::chrono::hours(1),
                                    [](ruvia::worker_timer_outcome) {}) ==
                                ruvia::worker_timer_schedule_status::scheduled;
        pending.cancel();
    });
    if (!rescheduled_post.accepted()) {
        return false;
    }
    drive(second_context);
    retire(first, first_context);
    retire(second, second_context);
    return first_expired && second_expired && pending_rejected && cancelled_rescheduled;
}

bool timer_callback_failure_preserves_remaining_operations() {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().worker();
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
    attachment.stop();
    io_context.restart();
    attachment.run();
    return success;
}

// A completion that re-arms an already-due deadline must not keep one timer
// pass running: other handlers, such as I/O and stop controls, run between
// rounds.
bool self_rescheduling_due_timer_yields_to_other_handlers() {
    constexpr int round_limit = 256;
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().worker();
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
    attachment.stop();
    io_context.restart();
    attachment.run();
    return rounds_before_handler >= 1 && rounds_before_handler < round_limit;
}

bool self_rescheduling_due_timer_preserves_other_expirations() {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
    const auto worker = attachment.loop().worker();
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
        } else {
            // The last expected expiration retires the loop so run() returns.
            attachment.stop();
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
    return order == "ABAA";
}

struct stop_during_move_state final {
    ruvia::event_loop_attachment* attachment_;
    int trigger_;
    int moves_{};
    int completions_{};
    bool stopped_{};
    bool cancelled_{};
};

struct stop_during_move_callback final {
    stop_during_move_state* state_;
    explicit stop_during_move_callback(stop_during_move_state& value) noexcept
        : state_(&value) {}
    stop_during_move_callback(const stop_during_move_callback&) = delete;
    stop_during_move_callback(stop_during_move_callback&& other) noexcept
        : state_(other.state_) {
        if (++state_->moves_ == state_->trigger_) {
            state_->stopped_ = true;
            state_->attachment_->stop();
        }
    }
    void operator()(ruvia::worker_timer_outcome outcome) {
        ++state_->completions_;
        state_->cancelled_ = outcome == ruvia::worker_timer_outcome::cancelled;
    }
};

// stop() cancels the worker's timers on its thread after it returns, so the
// registration is inspected once a sleep proves the timer queue has stopped.
ruvia::task<void> schedule_with_stop_during_callback_move(
    ruvia::event_loop_attachment& attachment, stop_during_move_state& current, bool& success) {
    const auto loop = attachment.loop();
    const auto& worker = loop.worker();
    ruvia::worker_timer_registration registration;
    ruvia::move_only_function<void(ruvia::worker_timer_outcome)> completion{stop_during_move_callback(current)};
    current.moves_ = 0;
    const auto result = worker.schedule_timer(
        registration, std::chrono::steady_clock::now() + std::chrono::hours(1), std::move(completion));
    if (!current.stopped_) {
        attachment.stop();
    }
    static_cast<void>(co_await ruvia::sleep_for(worker, std::chrono::hours(1)));
    bool reusable = false;
    try {
        reusable = worker.schedule_timer(registration, std::chrono::steady_clock::now(),
                       [](ruvia::worker_timer_outcome) {}) ==
                   ruvia::worker_timer_schedule_status::worker_stopping;
    } catch (const std::logic_error&) {
    }
    success = reusable &&
              ((result == ruvia::worker_timer_schedule_status::worker_stopping && current.completions_ == 0) ||
                  (result == ruvia::worker_timer_schedule_status::scheduled &&
                      current.completions_ == 1 && current.cancelled_));
    registration.cancel();
}

bool timer_registration_survives_stop_during_callback_move() {
    for (int trigger = 1; trigger <= 12; ++trigger) {
        asio::io_context io_context;
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
        stop_during_move_state current{&attachment, trigger};
        bool success = false;
        ruvia::test::run_worker_tasks(attachment, schedule_with_stop_during_callback_move(attachment, current, success));
        if (!success) {
            return false;
        }
    }
    return true;
}

bool timer_registration_rejects_reentrant_acquire() {
    struct state final {
        const ruvia::worker_ref* worker_;
        ruvia::worker_timer_registration* registration_;
        int trigger_;
        int moves_{};
        bool attempted_{};
        bool inner_accepted_{};
        bool inner_rejected_{};
    };
    struct callback final {
        state* state_;
        explicit callback(state& value) noexcept
            : state_(&value) {}
        callback(const callback&) = delete;
        callback(callback&& other) noexcept
            : state_(other.state_) {
            if (++state_->moves_ != state_->trigger_) {
                return;
            }
            state_->attempted_ = true;
            try {
                state_->inner_accepted_ = state_->worker_->schedule_timer(*state_->registration_,
                                              std::chrono::steady_clock::now() + std::chrono::hours(1),
                                              [](ruvia::worker_timer_outcome) {}) == ruvia::worker_timer_schedule_status::scheduled;
            } catch (const std::logic_error&) {
                state_->inner_rejected_ = true;
            }
        }
        void operator()(ruvia::worker_timer_outcome) {}
    };
    for (int trigger = 1; trigger <= 12; ++trigger) {
        asio::io_context io_context;
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
        const auto loop = attachment.loop();
        const auto& worker = loop.worker();
        ruvia::worker_timer_registration registration;
        state current{&worker, &registration, trigger};
        bool success = false;
        asio::post(io_context, [&] {
            ruvia::move_only_function<void(ruvia::worker_timer_outcome)> completion{callback(current)};
            current.moves_ = 0;
            bool outer_accepted = false;
            bool outer_rejected = false;
            try {
                outer_accepted = worker.schedule_timer(registration,
                                     std::chrono::steady_clock::now() + std::chrono::hours(1), std::move(completion)) ==
                                 ruvia::worker_timer_schedule_status::scheduled;
            } catch (const std::logic_error&) {
                outer_rejected = true;
            }
            success = current.attempted_
                          ? (current.inner_accepted_ && outer_rejected) ||
                                (outer_accepted && current.inner_rejected_)
                          : outer_accepted;
            registration.cancel();
            attachment.stop();
        });
        io_context.run();
        if (!success) {
            return false;
        }
    }
    return true;
}

TEST(Ruvia, TimerDurationConversionsPreserveIntegerTicksAndSaturate) {
    EXPECT_TRUE(timer_duration_conversions_preserve_integer_ticks_and_saturate());
}

TEST(Ruvia, timer_fractional_duration_conversion_preserves_rounding_and_bounds) {
    using target_type = std::chrono::steady_clock::duration;
    using period_type = std::ratio_multiply<target_type::period, std::ratio<3, 5>>;
    using signed_type = std::chrono::duration<std::int64_t, period_type>;
    using unsigned_type = std::chrono::duration<std::uint64_t, period_type>;
    for (const auto& [input, expected] : {std::pair<std::int64_t, std::int64_t>{0, 0},
             {1, 1}, {4, 3}, {5, 3}, {-1, 0}, {-4, -2}, {-5, -3},
             {9'000'000'000'000'000'003LL, 5'400'000'000'000'000'002LL},
             {-9'000'000'000'000'000'003LL, -5'400'000'000'000'000'001LL}}) {
        SCOPED_TRACE(input);
        EXPECT_EQ(ruvia::saturating_duration_cast(signed_type(input)).count(), expected);
    }
    EXPECT_EQ(ruvia::saturating_duration_cast(unsigned_type(10'000'000'000'000'000'002ULL)).count(), 6'000'000'000'000'000'002LL);
    EXPECT_EQ(ruvia::saturating_duration_cast(unsigned_type::max()), target_type::max());
    EXPECT_EQ(ruvia::saturating_duration_cast(signed_type::min()).count(), -5'534'023'222'112'865'484LL);
}

TEST(Ruvia, timer_fractional_duration_conversion_avoids_intermediate_overflow) {
    using target_type = std::chrono::steady_clock::duration;
    using period_type = std::ratio_multiply<target_type::period, std::ratio<9'000'000'001LL, 9'000'000'000LL>>;
    using signed_type = std::chrono::duration<std::int64_t, period_type>;
    using unsigned_type = std::chrono::duration<std::uint64_t, period_type>;
    EXPECT_EQ(ruvia::saturating_duration_cast(signed_type(8'999'999'999LL)).count(), 9'000'000'000LL);
    EXPECT_EQ(ruvia::saturating_duration_cast(signed_type(-8'999'999'999LL)).count(), -8'999'999'999LL);
    EXPECT_EQ(ruvia::saturating_duration_cast(signed_type::max()), target_type::max());
    EXPECT_EQ(ruvia::saturating_duration_cast(signed_type::min()), target_type::min());
    EXPECT_EQ(ruvia::saturating_duration_cast(unsigned_type::max()), target_type::max());
}

TEST(Ruvia, timer_floating_duration_underflow_keeps_positive_minimum_tick) {
    using input_type = std::chrono::duration<long double, std::micro>;
    using target_type = std::chrono::steady_clock::duration;
    using fine_type = std::chrono::duration<long double, std::ratio_divide<target_type::period, std::kilo>>;
    const auto tiny = (std::numeric_limits<long double>::denorm_min)();
    ASSERT_GT(tiny, 0.0L);
    EXPECT_EQ(ruvia::ceil_milliseconds(input_type(tiny)), std::chrono::milliseconds(1));
    EXPECT_EQ(ruvia::saturating_duration_cast<std::chrono::milliseconds>(input_type(tiny)), std::chrono::milliseconds(1));
    EXPECT_EQ(ruvia::saturating_duration_cast(fine_type(tiny)), target_type(1));
    EXPECT_EQ(ruvia::saturating_duration_cast<std::chrono::milliseconds>(input_type(-tiny)), std::chrono::milliseconds::zero());
    EXPECT_EQ(ruvia::ceil_milliseconds(input_type(-tiny)), std::chrono::milliseconds::zero());
    EXPECT_EQ(ruvia::saturating_duration_cast<std::chrono::milliseconds>(input_type::zero()), std::chrono::milliseconds::zero());
}

TEST(Ruvia, TimerImmediateShutdownWorks) {
    EXPECT_TRUE(timer_immediate_shutdown_works());
}
TEST(Ruvia, TimerOffWorkerCancellationCanRaceWithShutdown) {
    EXPECT_TRUE(off_worker_cancellation_can_race_with_timer_shutdown());
}
TEST(Ruvia, TimerOffWorkerCancellationAfterContextStopDoesNotExpireLater) {
    EXPECT_TRUE(off_worker_cancellation_after_context_stop_does_not_expire_later());
}
TEST(Ruvia, TimerOffWorkerCancellationBeforeQueuedStopDoesNotInvokeCallback) {
    EXPECT_TRUE(off_worker_cancellation_before_queued_stop_timers_does_not_invoke_callback());
}
TEST(Ruvia, TimerExplicitCancellationNotifiesAfterQuietDestruction) {
    EXPECT_TRUE(explicit_timer_cancellation_notifies_after_quiet_destruction());
}
TEST(Ruvia, TimerOnWorkerTokenCancellationThenOwnerCancelWithdrawsCallback) {
    EXPECT_TRUE(on_worker_token_cancellation_followed_by_owner_cancel_withdraws_callback());
}
TEST(Ruvia, TimerStoppableSleepWorks) {
    EXPECT_TRUE(stoppable_sleep_works());
}
TEST(Ruvia, TimerTimedPrimitivesReportStop) {
    EXPECT_TRUE(timed_primitives_report_timer_stop());
}
TEST(Ruvia, TimerExpiredRegistrationReschedulesOnAnotherWorker) {
    EXPECT_TRUE(expired_registration_reschedules_on_another_worker());
}
TEST(Ruvia, TimerCallbackFailurePreservesRemainingOperations) {
    EXPECT_TRUE(timer_callback_failure_preserves_remaining_operations());
}
TEST(Ruvia, TimerRegistrationSurvivesStopDuringCallbackMove) {
    EXPECT_TRUE(timer_registration_survives_stop_during_callback_move());
}
TEST(Ruvia, TimerRegistrationRejectsReentrantAcquire) {
    EXPECT_TRUE(timer_registration_rejects_reentrant_acquire());
}
TEST(Ruvia, TimerSelfReschedulingYieldsToOtherHandlers) {
    EXPECT_TRUE(self_rescheduling_due_timer_yields_to_other_handlers());
}
TEST(Ruvia, TimerSelfReschedulingPreservesOtherExpirations) {
    EXPECT_TRUE(self_rescheduling_due_timer_preserves_other_expirations());
}

TEST(Ruvia, WorkerTimerLifecycle) {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().worker();
    bool success = false;
    asio::co_spawn(io_context, ruvia::as_awaitable(exercise(attachment, worker_value, success)),
        asio::detached);
    attachment.run();
    attachment.stop();
    EXPECT_TRUE(success);
}

namespace {

template <typename left_type, typename right_type>
concept timeout_orderable = requires(left_type left, right_type right) { left < right; };

template <typename left_type, typename right_type>
concept timeout_equatable = requires(left_type left, right_type right) { left == right; };

static_assert(std::totally_ordered<ruvia::timeout_duration>);
static_assert(!timeout_orderable<ruvia::timeout_duration, std::chrono::nanoseconds>);
static_assert(!timeout_orderable<std::chrono::nanoseconds, ruvia::timeout_duration>);
static_assert(!timeout_equatable<ruvia::timeout_duration, std::chrono::nanoseconds>);
static_assert(!timeout_equatable<std::chrono::nanoseconds, ruvia::timeout_duration>);

enum class deadline_kind : std::uint8_t { read,
    write };

bool deadline_lifecycle_transitions_are_exclusive() {
    using deadline_type = ruvia::deadline_lifecycle<deadline_kind>;
    deadline_type deadline;
    const auto now = deadline_type::clock_type::time_point{};
    if (deadline.kind() != nullptr || deadline.expired() || deadline.clear()) {
        return false;
    }

    deadline.arm(now + std::chrono::seconds(1), deadline_kind::read);
    if (deadline.kind() == nullptr || *deadline.kind() != deadline_kind::read ||
        deadline.expire(now).has_value() || deadline.expired()) {
        return false;
    }

    const auto expired_kind = deadline.expire(now + std::chrono::seconds(1));
    if (expired_kind != deadline_kind::read || !deadline.expired() || deadline.kind() == nullptr ||
        *deadline.kind() != deadline_kind::read || !deadline.clear()) {
        return false;
    }

    deadline.arm(now, deadline_kind::write);
    deadline.reset();
    if (deadline.kind() != nullptr || deadline.expired() || deadline.clear()) {
        return false;
    }

    ruvia::deadline_lifecycle<> unlabelled;
    if (unlabelled.kind() != nullptr || unlabelled.expired()) {
        return false;
    }
    unlabelled.arm(ruvia::deadline_lifecycle<>::clock_type::now());
    return unlabelled.kind() != nullptr &&
           unlabelled.expire(ruvia::deadline_lifecycle<>::clock_type::now()).has_value() &&
           unlabelled.expired() && unlabelled.clear();
}

bool operation_deadline_uses_one_absolute_deadline() {
    using deadline_type = ruvia::operation_deadline;
    const deadline_type unlimited;
    if (unlimited.deadline().has_value() || unlimited.remaining().has_value() || unlimited.expired() ||
        unlimited.timeout().has_value()) {
        return false;
    }

    const deadline_type expired(deadline_type::clock_type::time_point{});
    if (!expired.deadline().has_value() || !expired.expired() ||
        expired.remaining() != std::chrono::milliseconds::zero()) {
        return false;
    }

    const deadline_type active(std::optional<ruvia::operation_timeout>(std::chrono::seconds(1)));
    const auto deadline_value = active.deadline();
    const auto remaining = active.remaining();
    if (!deadline_value.has_value() || !remaining.has_value() || remaining->count() <= 0 ||
        *remaining > std::chrono::seconds(1)) {
        return false;
    }
    // Nested operations receive the same absolute deadline, not a fresh relative bound.
    const auto forwarded = active.timeout();
    if (!forwarded.has_value() || forwarded->deadline() != deadline_value || forwarded->duration().has_value()) {
        return false;
    }
    return active.constrained_by(std::chrono::seconds(2)).deadline() == deadline_value &&
           unlimited.constrained_by(std::chrono::seconds(2)).deadline().has_value() &&
           !unlimited.constrained_by(std::nullopt).deadline().has_value();
}

}  // namespace

TEST(Ruvia, DeadlineLifecycleTransitionsAreExclusive) {
    EXPECT_TRUE(deadline_lifecycle_transitions_are_exclusive());
}

TEST(Ruvia, OperationDeadlineUsesOneAbsoluteDeadline) {
    EXPECT_TRUE(operation_deadline_uses_one_absolute_deadline());
}

TEST(Ruvia, operation_timeout_is_valid_by_construction) {
    using clock_type = ruvia::operation_timeout::clock_type;
    static_assert(std::is_convertible_v<std::chrono::seconds, ruvia::operation_timeout>);
    static_assert(std::is_convertible_v<ruvia::timeout_duration, ruvia::operation_timeout>);
    static_assert(!std::is_convertible_v<clock_type::time_point, ruvia::operation_timeout>);
    static_assert(!std::is_constructible_v<ruvia::operation_timeout, std::chrono::duration<double>>);

    const ruvia::operation_timeout relative = std::chrono::nanoseconds(1);
    EXPECT_EQ(relative.duration(), std::chrono::milliseconds(1));
    EXPECT_FALSE(relative.deadline().has_value());
    EXPECT_EQ(ruvia::operation_timeout(ruvia::timeout_duration(std::chrono::seconds(2))).duration(),
        std::chrono::milliseconds(2000));
    EXPECT_THROW(ruvia::operation_timeout(std::chrono::milliseconds(0)), std::invalid_argument);
    EXPECT_THROW(ruvia::operation_timeout(std::chrono::nanoseconds(-1)), std::invalid_argument);
    EXPECT_THROW(ruvia::operation_timeout(ruvia::timeout_duration{}), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(ruvia::operation_options{.timeout_ = std::chrono::seconds(-1)}), std::invalid_argument);

    // An absolute deadline is accepted as given, including one that has passed.
    const ruvia::operation_timeout passed(clock_type::time_point{});
    EXPECT_EQ(passed.deadline(), clock_type::time_point{});
    EXPECT_FALSE(passed.duration().has_value());
    const auto start = clock_type::now();
    EXPECT_EQ(passed.deadline_from(start), clock_type::time_point{});
    EXPECT_EQ(relative.deadline_from(start), start + std::chrono::milliseconds(1));
    EXPECT_EQ(ruvia::operation_timeout(std::chrono::hours::max()).deadline_from(start), clock_type::time_point::max());

    EXPECT_FALSE(ruvia::operation_timeout::try_create(std::chrono::milliseconds(0)).has_value());
    EXPECT_FALSE(ruvia::operation_timeout::try_create(std::chrono::seconds(-3)).has_value());
    EXPECT_FALSE(ruvia::operation_timeout::try_create(ruvia::timeout_duration{}).has_value());
    EXPECT_EQ(ruvia::operation_timeout::try_create(std::chrono::microseconds(1500)),
        ruvia::operation_timeout(std::chrono::milliseconds(2)));
    EXPECT_EQ(ruvia::operation_timeout::try_create(ruvia::timeout_duration(std::chrono::seconds(1))),
        ruvia::operation_timeout(std::chrono::seconds(1)));
}

TEST(Ruvia, operation_timeout_overrides_only_tighten) {
    using clock_type = ruvia::operation_timeout::clock_type;
    // Two relative limits keep the shorter.
    auto merged = ruvia::override_operation_options({.timeout_ = std::chrono::seconds(5)}, {.timeout_ = std::chrono::seconds(1)});
    EXPECT_EQ(merged.timeout_, ruvia::operation_timeout(std::chrono::seconds(1)));
    merged = ruvia::override_operation_options({.timeout_ = std::chrono::seconds(1)}, {.timeout_ = std::chrono::seconds(5)});
    EXPECT_EQ(merged.timeout_, ruvia::operation_timeout(std::chrono::seconds(1)));
    // Two deadlines keep the earlier.
    const auto early = clock_type::now() + std::chrono::seconds(1);
    const auto late = early + std::chrono::seconds(10);
    merged = ruvia::override_operation_options(
        {.timeout_ = ruvia::operation_timeout(late)}, {.timeout_ = ruvia::operation_timeout(early)});
    EXPECT_EQ(merged.timeout_->deadline(), early);
    merged = ruvia::override_operation_options(
        {.timeout_ = ruvia::operation_timeout(early)}, {.timeout_ = ruvia::operation_timeout(late)});
    EXPECT_EQ(merged.timeout_->deadline(), early);
    // A mixed pair resolves the relative limit at merge time and keeps the earlier deadline.
    merged = ruvia::override_operation_options({.timeout_ = std::chrono::hours(1)}, {.timeout_ = ruvia::operation_timeout(early)});
    EXPECT_EQ(merged.timeout_->deadline(), early);
    const auto before = clock_type::now();
    merged = ruvia::override_operation_options({.timeout_ = ruvia::operation_timeout(late)}, {.timeout_ = std::chrono::milliseconds(5)});
    ASSERT_TRUE(merged.timeout_.has_value());
    ASSERT_TRUE(merged.timeout_->deadline().has_value());
    EXPECT_GE(*merged.timeout_->deadline(), before + std::chrono::milliseconds(5));
    EXPECT_LT(*merged.timeout_->deadline(), late);
    // An unspecified override inherits; a supplied token replaces.
    ruvia::stop_source source;
    merged = ruvia::override_operation_options({.timeout_ = std::chrono::seconds(1)}, {.stop_token_ = source.token()});
    EXPECT_EQ(merged.timeout_, ruvia::operation_timeout(std::chrono::seconds(1)));
    EXPECT_TRUE(merged.stop_token_.has_state());
    merged = ruvia::override_operation_options({}, {.timeout_ = std::chrono::seconds(1)});
    EXPECT_EQ(merged.timeout_, ruvia::operation_timeout(std::chrono::seconds(1)));
}

TEST(Ruvia, timeout_duration_checks_the_original_integer_domain) {
    using wide_milliseconds = std::chrono::duration<std::uint64_t, std::milli>;
    constexpr wide_milliseconds wide(std::numeric_limits<std::uint64_t>::max());
    constexpr ruvia::timeout_duration saturated(wide);
    static_assert(saturated.value() == std::chrono::milliseconds::max());
    static_assert(!std::is_convertible_v<ruvia::timeout_duration, std::chrono::milliseconds>);
    static_assert(!std::is_constructible_v<ruvia::timeout_duration, std::chrono::duration<double>>);

    ruvia::operation_options options{.timeout_ = wide};
    ASSERT_TRUE(options.timeout_.has_value());
    EXPECT_EQ(options.timeout_->duration(), std::chrono::milliseconds::max());

    options.timeout_ = std::chrono::nanoseconds(1);
    EXPECT_EQ(options.timeout_->duration(), std::chrono::milliseconds(1));

    using thirds = std::chrono::duration<std::int64_t, std::ratio<1, 3>>;
    EXPECT_EQ(ruvia::timeout_duration(thirds(7)).value(), std::chrono::milliseconds(2334));
    EXPECT_EQ(ruvia::timeout_duration(thirds(-7)).value(), std::chrono::milliseconds(-2333));
    using wide_hours = std::chrono::duration<std::int64_t, std::ratio<3600>>;
    EXPECT_EQ(ruvia::timeout_duration(wide_hours::min()).value(),
        std::chrono::milliseconds::min());
}

TEST(Ruvia, wide_positive_timeouts_remain_active_and_can_be_tightened) {
    using wide_milliseconds = std::chrono::duration<std::uint64_t, std::milli>;
    const wide_milliseconds wide(std::numeric_limits<std::uint64_t>::max());
    const ruvia::operation_deadline timeout{std::optional<ruvia::operation_timeout>(wide)};
    EXPECT_FALSE(timeout.expired());
    ASSERT_TRUE(timeout.remaining().has_value());
    EXPECT_GT(*timeout.remaining(), std::chrono::milliseconds::zero());

    const ruvia::operation_deadline unlimited;
    EXPECT_FALSE(unlimited.constrained_by(wide).expired());
    ruvia::deadline_lifecycle<> deadline;
    deadline.arm(*timeout.deadline());
    EXPECT_FALSE(deadline.expire(std::chrono::steady_clock::now()).has_value());
    EXPECT_TRUE(deadline.expire(std::chrono::steady_clock::time_point::max()).has_value());

    const auto tightened = ruvia::override_operation_options(
        {.timeout_ = wide}, {.timeout_ = std::chrono::nanoseconds(1)});
    ASSERT_TRUE(tightened.timeout_.has_value());
    EXPECT_EQ(tightened.timeout_->duration(), std::chrono::milliseconds(1));
    const auto retained = ruvia::override_operation_options(tightened, {.timeout_ = wide});
    EXPECT_EQ(retained.timeout_, tightened.timeout_);
}

TEST(Ruvia, timer_conversions_do_not_narrow_before_saturation) {
    using clock_type = std::chrono::steady_clock;
    using wide_ticks = std::chrono::duration<std::uint64_t, clock_type::duration::period>;
    const wide_ticks ticks(std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(ruvia::saturating_deadline(clock_type::time_point{}, ticks),
        clock_type::time_point::max());

    using wide_milliseconds = std::chrono::duration<std::uint64_t, std::milli>;
    const wide_milliseconds milliseconds(std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(ruvia::ceil_milliseconds(milliseconds),
        std::chrono::milliseconds::max());
    EXPECT_EQ(ruvia::ceil_milliseconds(std::chrono::nanoseconds(1)),
        std::chrono::milliseconds(1));
}

namespace ruvia::test::task_completion_tests {

struct allocation_counts final {
    std::size_t allocations_{0};
    std::size_t deallocations_{0};
};

template <typename t_type>
class counting_allocator {
public:
    using value_type = t_type;

    explicit counting_allocator(allocation_counts& counts) noexcept
        : counts_(&counts) {}

    template <typename u_type>
    counting_allocator(const counting_allocator<u_type>& other) noexcept
        : counts_(other.counts()) {}

    [[nodiscard]] t_type* allocate(std::size_t count) {
        ++counts_->allocations_;
        return std::allocator<t_type>{}.allocate(count);
    }

    void deallocate(t_type* pointer, std::size_t count) noexcept {
        ++counts_->deallocations_;
        std::allocator<t_type>{}.deallocate(pointer, count);
    }

    [[nodiscard]] allocation_counts* counts() const noexcept {
        return counts_;
    }

    template <typename u_type>
    [[nodiscard]] bool operator==(const counting_allocator<u_type>& other) const noexcept {
        return counts_ == other.counts();
    }

private:
    allocation_counts* counts_;
};

struct frame_allocation final {
    explicit frame_allocation(allocation_counts& counts)
        : values_(256, 0, counting_allocator<int>(counts)) {}

    std::vector<int, counting_allocator<int>> values_;
};

template <typename result_type>
ruvia::task<result_type> cold_frame(frame_allocation frame, bool& ran) {
    ran = true;
    if constexpr (std::is_void_v<result_type>) {
        co_return;
    } else {
        co_return static_cast<int>(frame.values_.size());
    }
}

template <typename result_type>
bool check_cold_frame(bool cross_thread) {
    allocation_counts counts;
    bool ran = false;
    std::optional<ruvia::task<result_type>> task_value;
    auto create = [&] {
        task_value.emplace(cold_frame<result_type>(frame_allocation(counts), ran));
    };
    if (cross_thread) {
        std::thread(create).join();
    } else {
        create();
    }
    if (ran || counts.allocations_ == counts.deallocations_) {
        return false;
    }
    if (cross_thread) {
        std::thread([&] { task_value.reset(); }).join();
    } else {
        task_value.reset();
    }
    return !ran && counts.allocations_ == counts.deallocations_;
}

}  // namespace ruvia::test::task_completion_tests

TEST(Ruvia, ColdTaskFrameSameThread) {
    EXPECT_TRUE(ruvia::test::task_completion_tests::check_cold_frame<void>(false));
    EXPECT_TRUE(ruvia::test::task_completion_tests::check_cold_frame<int>(false));
}

TEST(Ruvia, ColdTaskFrameCrossThread) {
    EXPECT_TRUE(ruvia::test::task_completion_tests::check_cold_frame<void>(true));
    EXPECT_TRUE(ruvia::test::task_completion_tests::check_cold_frame<int>(true));
}
