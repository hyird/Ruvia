#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <future>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include "ruvia/core/detail/io/asio_await.h"
#include "ruvia/core/detail/worker/worker_dispatcher.h"
#include "ruvia/core/detail/worker/worker_wait_awaiter.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"

namespace {

class throwing_move final {
public:
    explicit throwing_move(int value) noexcept
        : value_(value) {}

    throwing_move(const throwing_move&) = delete;
    throwing_move& operator=(const throwing_move&) = delete;
    // This fixture intentionally models a move that can throw.
    throwing_move(throwing_move&& other) noexcept(false) {
        if (throw_on_move_) {
            throw std::runtime_error("requested move failure");
        }
        value_ = std::exchange(other.value_, 0);
    }

    [[nodiscard]] int value() const noexcept {
        return value_;
    }

    static inline bool throw_on_move_{false};

private:
    int value_{0};
};

bool discriminated_wait_state_works() {
    ruvia::detail::worker_wait_await_state_type<int> early;
    if (early.complete(ruvia::detail::worker_wait_result_access::value(3)) ||
        early.suspend(std::noop_coroutine())) {
        return false;
    }
    const auto early_result = early.take_value();
    if (!early_result.has_value() || early_result.value() != 3) {
        return false;
    }

    ruvia::detail::worker_wait_await_state_type<int> suspended;
    const auto continuation = std::noop_coroutine();
    if (!suspended.suspend(continuation) ||
        !suspended.complete(ruvia::detail::worker_wait_result_access::outcome<int>(
            ruvia::worker_wait_status::timed_out)) ||
        suspended.continuation() != continuation) {
        return false;
    }
    const auto suspended_result = suspended.take_value();
    if (suspended_result.status() != ruvia::worker_wait_status::timed_out) {
        return false;
    }

    ruvia::detail::worker_wait_await_state_type<throwing_move> recovering;
    auto failed_result = ruvia::detail::worker_wait_result_access::value(throwing_move(5));
    throwing_move::throw_on_move_ = true;
    bool move_failed = false;
    try {
        static_cast<void>(recovering.complete(std::move(failed_result)));
    } catch (const std::runtime_error&) {
        move_failed = true;
    }
    throwing_move::throw_on_move_ = false;
    if (!move_failed ||
        recovering.complete(ruvia::detail::worker_wait_result_access::value(throwing_move(7)))) {
        return false;
    }
    const auto recovered = recovering.take_value();
    return recovered.has_value() && recovered.value().value() == 7;
}

bool saturating_timer_deadline_works() {
    using clock_type = std::chrono::steady_clock;
    using ruvia::worker_timer_saturating_deadline;

    const auto ordinary_now = clock_type::time_point(clock_type::duration(100));
    if (worker_timer_saturating_deadline(ordinary_now, clock_type::duration(25)) !=
        clock_type::time_point(clock_type::duration(125))) {
        return false;
    }

    const auto near_maximum = clock_type::time_point::max() - clock_type::duration(5);
    if (worker_timer_saturating_deadline(near_maximum, clock_type::duration(10)) !=
        clock_type::time_point::max()) {
        return false;
    }

    // The old direct `now + duration` expression overflowed for this public
    // input and could turn an effectively infinite wait into an expired timer.
    return worker_timer_saturating_deadline(ordinary_now, clock_type::duration::max()) ==
           clock_type::time_point::max();
}

bool saturating_timer_duration_cast_works() {
    using target_type = std::chrono::steady_clock::duration;
    using ruvia::worker_timer_saturating_duration_cast;

    const auto ordinary = worker_timer_saturating_duration_cast(std::chrono::microseconds(1500));
    if (ordinary != std::chrono::duration_cast<target_type>(std::chrono::microseconds(1500))) {
        return false;
    }

    using unsigned_seconds_type = std::chrono::duration<std::uint64_t>;
    if (worker_timer_saturating_duration_cast(unsigned_seconds_type::max()) != target_type::max()) {
        return false;
    }
    if (ruvia::worker_timer_deadline_after(std::chrono::milliseconds::max()) !=
        std::chrono::steady_clock::time_point::max()) {
        return false;
    }

    using floating_seconds_type = std::chrono::duration<long double>;
    return worker_timer_saturating_duration_cast(
               floating_seconds_type(std::numeric_limits<long double>::infinity())) == target_type::max() &&
           worker_timer_saturating_duration_cast(
               floating_seconds_type(-std::numeric_limits<long double>::infinity())) == target_type::min() &&
           worker_timer_saturating_duration_cast(
               floating_seconds_type(std::numeric_limits<long double>::quiet_NaN())) == target_type::zero();
}

bool timer_immediate_shutdown_works() {
    asio::io_context io_context;
    for (int attempt_value = 0; attempt_value < 32; ++attempt_value) {
        io_context.restart();
        const auto dispatcher = std::make_shared<ruvia::detail::worker_dispatcher>(io_context, 2);
        const auto worker_value = ruvia::detail::worker_handle_access::make(dispatcher);
        ruvia::worker_timer_registration registration;
        std::promise<void> stopped;
        auto stopped_ready = stopped.get_future();
        asio::post(io_context, [&] {
            (worker_value).schedule_timer(registration, std::chrono::steady_clock::now() + std::chrono::hours(1), [](ruvia::worker_timer_outcome) {});
            dispatcher->stop_timers();
            asio::post(io_context, [&] {
                stopped.set_value();
                io_context.stop();
            });
        });
        std::thread worker_thread([&] { io_context.run(); });
        stopped_ready.get();
        worker_thread.join();
        dispatcher->detach_context();
    }
    return true;
}

bool stopped_dispatcher_can_outlive_context() {
    std::shared_ptr<ruvia::detail::worker_dispatcher> dispatcher;
    ruvia::worker_handle worker;
    {
        asio::io_context io_context;
        dispatcher = std::make_shared<ruvia::detail::worker_dispatcher>(io_context, 2);
        worker = ruvia::detail::worker_handle_access::make(dispatcher);
        dispatcher->stop_timers();
    }
    dispatcher.reset();
    worker = ruvia::worker_handle{};
    return true;
}

bool timer_registration_reset_after_stop_does_not_queue_cancellation() {
    asio::io_context io_context;
    const auto dispatcher = std::make_shared<ruvia::detail::worker_dispatcher>(io_context, 2);
    const auto worker_value = ruvia::detail::worker_handle_access::make(dispatcher);
    auto registration = std::make_unique<ruvia::worker_timer_registration>();
    std::size_t cancelled = 0;
    std::size_t expired = 0;

    asio::post(io_context, [&] {
        (worker_value).schedule_timer(*registration, std::chrono::steady_clock::now() + std::chrono::hours(1), [&](ruvia::worker_timer_outcome outcome) {
            if (outcome == ruvia::worker_timer_outcome::cancelled) {
                ++cancelled;
            } else if (outcome == ruvia::worker_timer_outcome::expired) {
                ++expired;
            }
        });
        dispatcher->stop_timers();
        io_context.stop();
    });
    io_context.run();
    if (cancelled != 1 || expired != 0) {
        dispatcher->detach_context();
        return false;
    }

    io_context.restart();
    while (io_context.poll() != 0) {
    }

    registration.reset();

    io_context.restart();
    const auto queued_handlers = io_context.poll();
    dispatcher->detach_context();
    return queued_handlers == 0 && cancelled == 1 && expired == 0;
}

bool off_worker_cancellation_can_race_with_timer_shutdown() {
    for (int attempt_value = 0; attempt_value < 64; ++attempt_value) {
        asio::io_context io_context;
        const auto dispatcher = std::make_shared<ruvia::detail::worker_dispatcher>(io_context, 8);
        const auto worker_value = ruvia::detail::worker_handle_access::make(dispatcher);
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
            dispatcher->stop_timers();
            io_context.stop();
        });

        std::thread worker_thread([&] { io_context.run(); });
        std::thread cancelling_thread([&] {
            start.arrive_and_wait();
            for (int call = 0; call < 64; ++call) {
                cancellation.cancel();
            }
        });
        cancelling_thread.join();
        worker_thread.join();
        dispatcher->detach_context();
        if (completions.load(std::memory_order_relaxed) != 1) {
            return false;
        }
    }
    return true;
}

bool off_worker_cancellation_after_context_stop_does_not_expire_later() {
    asio::io_context io_context;
    auto dispatcher = std::make_shared<ruvia::detail::worker_dispatcher>(io_context, 2);
    auto worker_value = ruvia::detail::worker_handle_access::make(dispatcher);
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
    io_context.run();

    // Destruction is a quiet cancellation issued outside the worker while the
    // context is stopped. It still has to remove the active timer slot;
    // otherwise a later restart can fire a callback owned by an already-destroyed
    // registration.
    registration.reset();

    io_context.restart();
    io_context.run_for(std::chrono::milliseconds(20));
    dispatcher->detach_context();
    return !expired && !cancelled;
}

bool explicit_timer_cancellation_notifies_after_quiet_destruction() {
    asio::io_context io_context;
    const auto dispatcher = std::make_shared<ruvia::detail::worker_dispatcher>(io_context, 2);
    const auto worker_value = ruvia::detail::worker_handle_access::make(dispatcher);
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
    io_context.run();

    io_context.restart();
    while (io_context.poll() != 0) {
    }
    dispatcher->detach_context();
    return cancelled;
}

ruvia::task<void> mark_after_sleep(
    ruvia::worker_handle worker_value, bool& completed, bool& reported_elapsed) {
    reported_elapsed = co_await ruvia::sleep_for(worker_value, std::chrono::hours(1)) ==
                       ruvia::timer_sleep_result::elapsed;
    completed = true;
}

ruvia::task<void> exercise(const std::shared_ptr<ruvia::detail::worker_dispatcher>& dispatcher,
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
    dispatcher->stop_timers();
    co_await scope.join();
    // A normal sleep reports elapsed; a shutdown-cancelled sleep resumes but
    // reports not-elapsed so a periodic loop can stop instead of re-sleeping.
    success = expired && cancelled && first_sleep_elapsed && cancelled_sleep_resumed &&
              !cancelled_sleep_reported_elapsed;
}

ruvia::task<void> exercise_slot_reuse(ruvia::worker_handle worker_value, bool& success) {
    constexpr std::size_t timer_count = 256;
    auto registrations = std::make_unique<ruvia::worker_timer_registration[]>(timer_count);
    std::size_t cancelled = 0;
    std::size_t expired = 0;

    for (std::size_t index = 0; index < timer_count; ++index) {
        (worker_value).schedule_timer(registrations[index], std::chrono::steady_clock::now() + std::chrono::hours(1), [&cancelled](ruvia::worker_timer_outcome outcome) {
            if (outcome == ruvia::worker_timer_outcome::cancelled) {
                ++cancelled;
            }
        });
    }

    bool rejected_active_reuse = false;
    try {
        (worker_value).schedule_timer(registrations[0], std::chrono::steady_clock::now(), [](ruvia::worker_timer_outcome) {});
    } catch (const std::logic_error&) {
        rejected_active_reuse = true;
    }

    for (std::size_t index = 0; index < timer_count; ++index) {
        registrations[index].cancel();
    }
    // Reuse every slot before the stale heap entries are popped. Generation
    // validation must prevent the old entries from cancelling or expiring the
    // replacements (ABA), even when cancellation compaction also runs.
    for (std::size_t index = 0; index < timer_count; ++index) {
        (worker_value).schedule_timer(registrations[index], std::chrono::steady_clock::now(), [&expired](ruvia::worker_timer_outcome outcome) {
            if (outcome == ruvia::worker_timer_outcome::expired) {
                ++expired;
            }
        });
    }
    static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1)));
    success = rejected_active_reuse && cancelled == timer_count && expired == timer_count;
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
    ruvia::detail::worker_handle_access::defer(worker_value, [&inflight] { inflight.request_stop(); });
    const auto parked = std::chrono::steady_clock::now();
    if (co_await ruvia::sleep_for(worker_value, std::chrono::seconds(30), inflight.token()) !=
        ruvia::timer_sleep_result::stop_requested) {
        co_return;
    }
    if (std::chrono::steady_clock::now() - parked > std::chrono::seconds(5)) {
        co_return;
    }

    // The stop callback can run on an arbitrary thread. It may only post the
    // timer generation back to this worker; the dispatcher borrow stays valid
    // through callback teardown without copying shared ownership into the
    // awaiter.
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
    // Its own io_context and dispatcher: other cases in this file exercise
    // worker shutdown, which stops timers, and a parked sleep sharing that
    // dispatcher would be cancelled by them rather than by its own token.
    asio::io_context io_context;
    const auto dispatcher = std::make_shared<ruvia::detail::worker_dispatcher>(io_context, 8);
    const auto worker_value = ruvia::detail::worker_handle_access::make(dispatcher);
    bool success = false;
    asio::co_spawn(io_context,
        ruvia::detail::task_as_awaitable(exercise_stoppable_sleep(worker_value, success)), asio::detached);
    io_context.run();
    dispatcher->close();
    return success;
}

int main() {
    if (!discriminated_wait_state_works() || !saturating_timer_deadline_works() ||
        !saturating_timer_duration_cast_works() || !timer_immediate_shutdown_works() ||
        !stopped_dispatcher_can_outlive_context() ||
        !timer_registration_reset_after_stop_does_not_queue_cancellation() ||
        !off_worker_cancellation_can_race_with_timer_shutdown() ||
        !off_worker_cancellation_after_context_stop_does_not_expire_later() ||
        !explicit_timer_cancellation_notifies_after_quiet_destruction() || !stoppable_sleep_works()) {
        return 1;
    }
    asio::io_context io_context;
    const auto dispatcher = std::make_shared<ruvia::detail::worker_dispatcher>(io_context, 8);
    const auto worker_value = ruvia::detail::worker_handle_access::make(dispatcher);
    bool success = false;
    bool slot_reuse_success = false;
    asio::co_spawn(io_context, ruvia::detail::task_as_awaitable(exercise(dispatcher, worker_value, success)),
        asio::detached);
    asio::co_spawn(io_context,
        ruvia::detail::task_as_awaitable(exercise_slot_reuse(worker_value, slot_reuse_success)),
        asio::detached);
    io_context.run();
    dispatcher->close();
    return success && slot_reuse_success ? 0 : 1;
}
