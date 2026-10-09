#include "ruvia/core/blocking_pool.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_runtime_context.h"

#include "worker_task_fixture.h"

namespace {

using ruvia::blocking_pool;
using ruvia::blocking_pool_options;

[[nodiscard]] bool blocking_pool_defaults_are_bounded_by_cpu_policy() {
    blocking_pool pool;
    const bool valid = pool.thread_count() >= 2 && pool.thread_count() <= 8 &&
                       pool.queue_capacity() == pool.thread_count() * 64;
    pool.stop();
    pool.join();
    return valid;
}
using ruvia::blocking_status;
using ruvia::blocking_submit_status;
using ruvia::task;
using ruvia::worker_handle;

// A task the pool runs on its own thread: it reports that it started, then
// holds its thread until the test releases it.
struct thread_gate final {
    std::binary_semaphore started_{0};
    std::binary_semaphore release_{0};

    void occupy(blocking_pool& pool) {
        const auto submitted = pool.submit([this] {
            started_.release();
            release_.acquire();
        });
        if (submitted != blocking_submit_status::accepted) {
            std::terminate();
        }
        started_.acquire();
    }
};

struct reentrant_discard_state final {
    blocking_pool* pool_{nullptr};
    std::atomic_int destroyed_{0};
    std::atomic_int ran_{0};
    std::atomic_int rejected_{0};
};

struct reentrant_discarded_task final {
    reentrant_discard_state* state_{nullptr};

    reentrant_discarded_task() = default;
    explicit reentrant_discarded_task(reentrant_discard_state& state_value)
        : state_(&state_value) {}
    reentrant_discarded_task(const reentrant_discarded_task&) = delete;
    reentrant_discarded_task& operator=(const reentrant_discarded_task&) = delete;
    reentrant_discarded_task(reentrant_discarded_task&& other) noexcept
        : state_(std::exchange(other.state_, nullptr)) {}

    ~reentrant_discarded_task() {
        if (state_ == nullptr) {
            return;
        }
        state_->destroyed_.fetch_add(1, std::memory_order_relaxed);
        static_cast<void>(state_->pool_->stats());
        state_->pool_->stop();
        if (state_->pool_->submit([] {}) == blocking_submit_status::pool_stopped) {
            state_->rejected_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void operator()() const noexcept {
        state_->ran_.fetch_add(1, std::memory_order_relaxed);
    }
};

task<void> exercise_results(blocking_pool& pool, worker_handle worker_value, bool& success) {
    auto value = co_await ruvia::try_run_blocking(pool, worker_value, [] { return 42; });
    if (!value.completed() || std::move(value).value() != 42) {
        co_return;
    }

    auto thrown = co_await ruvia::try_run_blocking(
        pool, worker_value, []() -> int { throw std::runtime_error("blocking work failed"); });
    if (thrown.status() != blocking_status::completed || !thrown.failed()) {
        co_return;
    }
    bool rethrown = false;
    try {
        static_cast<void>(std::move(thrown).value());
    } catch (const std::runtime_error&) {
        rethrown = true;
    }
    if (!rethrown) {
        co_return;
    }

    std::atomic_bool ran{false};
    auto empty = co_await ruvia::try_run_blocking(pool, worker_value, [flag = &ran] { flag->store(true); });
    if (!empty.completed() || !ran.load()) {
        co_return;
    }
    static_cast<void>(std::move(empty).value());

    // A moved-only result travels back by move, not by copy.
    auto owned =
        co_await ruvia::try_run_blocking(pool, worker_value, [] { return std::make_unique<int>(7); });
    if (!owned.completed()) {
        co_return;
    }
    const auto pointer = std::move(owned).value();
    if (pointer == nullptr || *pointer != 7) {
        co_return;
    }

    const auto direct = co_await ruvia::run_blocking(pool, worker_value, [] { return 11; });
    bool direct_rethrew = false;
    try {
        static_cast<void>(co_await ruvia::run_blocking(
            pool, worker_value, []() -> int { throw std::runtime_error("direct blocking failure"); }));
    } catch (const std::runtime_error& error) {
        direct_rethrew = std::string_view(error.what()) == "direct blocking failure";
    }
    success = direct == 11 && direct_rethrew;
}

task<void> exercise_cancellation(blocking_pool& pool, worker_handle worker_value, thread_gate& gate_value,
    ruvia::stop_source& source_value, std::atomic_bool& callable_finished, bool& success) {
    auto result_value =
        co_await ruvia::try_run_blocking(pool, worker_value, std::chrono::seconds(30), source_value.token(), [&] {
            gate_value.started_.release();
            gate_value.release_.acquire();
            callable_finished.store(true, std::memory_order_release);
            return 12;
        });
    if (result_value.status() != blocking_status::cancelled || result_value.completed() || result_value.failed()) {
        gate_value.release_.release();
        co_return;
    }
    try {
        static_cast<void>(std::move(result_value).value());
    } catch (const ruvia::blocking_operation_rejected& error) {
        success = error.status() == blocking_status::cancelled;
    }
    gate_value.release_.release();
}

// A wedged callable must not pin its caller forever: the wait has a deadline,
// even though the pool thread stays occupied until the callable returns.
task<void> exercise_timeout(
    blocking_pool& pool, worker_handle worker_value, thread_gate& gate_value, bool& success) {
    auto timed_out =
        co_await ruvia::try_run_blocking(pool, worker_value, std::chrono::milliseconds(20), [&gate_value] {
            gate_value.release_.acquire();
            return 1;
        });
    if (timed_out.status() != blocking_status::timed_out || timed_out.completed()) {
        co_return;
    }
    bool rejected = false;
    try {
        static_cast<void>(std::move(timed_out).value());
    } catch (const ruvia::blocking_operation_rejected& error) {
        rejected = error.status() == blocking_status::timed_out;
    }
    gate_value.release_.release();

    // A deadline that is not reached behaves exactly like the untimed wait.
    auto in_time =
        co_await ruvia::try_run_blocking(pool, worker_value, std::chrono::seconds(30), [] { return 9; });
    success = rejected && in_time.completed() && std::move(in_time).value() == 9;
}

task<void> exercise_saturating_timeout(
    blocking_pool& pool, worker_handle worker_value, thread_gate& gate_value, bool& success) {
    // Keep the pool occupied so a wrapped timeout cannot hide behind a task
    // that happens to finish before the receiver arms its deadline.
    std::thread releaser([&pool, &gate_value] {
        while (pool.stats().queued_ == 0) {
            std::this_thread::yield();
        }
        gate_value.release_.release();
    });

    auto result_value =
        co_await ruvia::try_run_blocking(pool, worker_value, std::chrono::hours::max(), [] { return 17; });
    releaser.join();
    success = result_value.completed() && std::move(result_value).value() == 17;
}

task<void> count_on_worker(std::atomic_int& order, int& observed_value) {
    observed_value = order.fetch_add(1);
    co_return;
}

task<void> block_then_count(blocking_pool& pool, worker_handle worker_value, std::atomic_int& order,
    int& observed_value, bool& completed) {
    auto result_value = co_await ruvia::try_run_blocking(
        pool, worker_value, [] { std::this_thread::sleep_for(std::chrono::milliseconds(50)); });
    completed = result_value.completed();
    observed_value = order.fetch_add(1);
}

// The point of the pool: while one handler waits on blocking work, the worker
// keeps running other coroutines instead of sitting inside the blocking call.
task<void> exercise_worker_stays_free(blocking_pool& pool, worker_handle worker_value, bool& success) {
    std::atomic_int order{0};
    int blocking_order = -1;
    int free_order = -1;
    bool blocking_completed = false;

    ruvia::task_scope scope(worker_value);
    scope.spawn(block_then_count(pool, worker_value, order, blocking_order, blocking_completed));
    scope.spawn(count_on_worker(order, free_order));
    co_await scope.join();

    success = blocking_completed && free_order == 0 && blocking_order == 1;
}

// A pool that stops must not leave a suspended handler waiting forever: tasks
// discarded from the queue still answer their waiter.
task<void> exercise_stopped_pool(
    blocking_pool& pool, worker_handle worker_value, thread_gate& gate_value, bool& success) {
    std::thread stopper([&pool] {
        while (pool.stats().queued_ == 0) {
            std::this_thread::yield();
        }
        pool.stop();
    });

    auto result_value = co_await ruvia::try_run_blocking(pool, worker_value, [] { return 1; });
    stopper.join();
    gate_value.release_.release();

    if (result_value.status() != blocking_status::pool_stopped || result_value.completed()) {
        co_return;
    }
    bool rejected = false;
    try {
        static_cast<void>(std::move(result_value).value());
    } catch (const ruvia::blocking_operation_rejected& error) {
        rejected = error.status() == blocking_status::pool_stopped;
    }
    // A stopped pool refuses new work rather than queueing it forever.
    auto refused = co_await ruvia::try_run_blocking(pool, worker_value, [] { return 2; });
    // A stopping pool is shutdown accounting, never the overload signal an
    // operator sizes the pool from.
    const auto stats = pool.stats();
    success = rejected && refused.status() == blocking_status::pool_stopped && stats.rejected_ == 0 &&
              stats.discarded_ >= 2;
}

task<void> exercise_queue_full(
    blocking_pool& pool, worker_handle worker_value, thread_gate& gate_value, bool& success) {
    // One thread is occupied and the single queue slot is taken, so this one
    // has nowhere to go.
    const auto queued = pool.submit([] {});
    auto result_value = co_await ruvia::try_run_blocking(pool, worker_value, [] { return 1; });
    gate_value.release_.release();
    const auto stats = pool.stats();
    success = queued == blocking_submit_status::accepted &&
              result_value.status() == blocking_status::queue_full && stats.rejected_ == 1 &&
              stats.discarded_ == 0;
}

task<void> exercise_worker_stopping(
    ruvia::worker_runtime_context& runtime, blocking_pool& pool,
    worker_handle worker_value, thread_gate& gate_value, bool& success) {
    asio::post(runtime.io_context(), [&runtime] { runtime.close(); });
    auto result_value = co_await ruvia::try_run_blocking(pool, worker_value, [&gate_value] {
        gate_value.release_.acquire();
        return 5;
    });
    success = result_value.status() == blocking_status::worker_stopping && !result_value.completed() &&
              !result_value.failed();
}

// Offloading from a worker that has ALREADY stopped is the same shutdown
// outcome, not an exception the caller never asked for.
task<void> exercise_stopped_worker(
    blocking_pool& pool, worker_handle worker_value, std::atomic_bool& ran, bool& success) {
    auto result_value = co_await ruvia::try_run_blocking(pool, worker_value, [flag = &ran] {
        flag->store(true);
        return 1;
    });
    success = result_value.status() == blocking_status::worker_stopping;
}

bool test_destruction_does_not_join_running_callable() {
    auto started_promise = std::make_shared<std::promise<void>>();
    auto started = started_promise->get_future();
    auto finished_promise = std::make_shared<std::promise<void>>();
    auto finished = finished_promise->get_future();
    std::promise<void> release_promise;
    auto release = release_promise.get_future().share();
    std::promise<void> destroyed_promise;
    auto destroyed = destroyed_promise.get_future();

    auto pool = std::make_unique<blocking_pool>(blocking_pool_options{.thread_count_ = 1});
    if (pool->submit(
            [started = std::move(started_promise), finished = std::move(finished_promise), release] {
                started->set_value();
                release.wait();
                finished->set_value();
            }) != blocking_submit_status::accepted) {
        return false;
    }
    started.wait();

    std::thread destroyer([pool = std::move(pool), &destroyed_promise]() mutable {
        pool.reset();
        destroyed_promise.set_value();
    });
    const bool returned_before_callable =
        destroyed.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;

    release_promise.set_value();
    destroyer.join();
    const bool callable_finished =
        finished.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
    return returned_before_callable && callable_finished;
}

bool test_join_stops_and_waits_for_running_callable() {
    auto started_promise = std::make_shared<std::promise<void>>();
    auto started = started_promise->get_future();
    std::promise<void> release_promise;
    auto release = release_promise.get_future().share();
    std::promise<void> joined_promise;
    auto joined = joined_promise.get_future();

    blocking_pool pool(blocking_pool_options{.thread_count_ = 1});
    if (pool.submit([started = std::move(started_promise), release] {
            started->set_value();
            release.wait();
        }) != blocking_submit_status::accepted) {
        return false;
    }
    started.wait();

    std::thread joiner([&pool, &joined_promise] {
        pool.join();
        joined_promise.set_value();
    });
    const bool join_waited =
        joined.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout;

    release_promise.set_value();
    joiner.join();
    return join_waited && joined.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
}

bool test_join_rejects_pool_thread_before_stopping() {
    blocking_pool pool(blocking_pool_options{.thread_count_ = 1, .queue_capacity_ = 1});
    std::promise<bool> completed;
    auto result_value = completed.get_future();
    if (pool.submit([&] {
            bool rejected = false;
            try {
                pool.join();
            } catch (const std::logic_error& error) {
                rejected = std::string_view(error.what()) ==
                           "cannot join a blocking pool from one of its threads";
            }
            const auto still_accepting = pool.submit([] {}) == blocking_submit_status::accepted;
            completed.set_value(rejected && still_accepting);
        }) != blocking_submit_status::accepted) {
        return false;
    }
    const bool rejected = result_value.get();
    pool.join();
    return rejected;
}

bool test_stop_drains_without_running_queued_tasks() {
    constexpr int queued_task_count = 3;
    thread_gate gate;
    reentrant_discard_state state;
    blocking_pool pool(blocking_pool_options{.thread_count_ = 1, .queue_capacity_ = queued_task_count});
    state.pool_ = &pool;
    gate.occupy(pool);

    for (int i = 0; i < queued_task_count; ++i) {
        if (pool.submit(reentrant_discarded_task(state)) !=
            blocking_submit_status::accepted) {
            gate.release_.release();
            pool.join();
            return false;
        }
    }

    pool.stop();
    const auto stopped = pool.stats();
    gate.release_.release();
    pool.join();
    const auto finished = pool.stats();
    return stopped.queued_ == 0 && stopped.running_ == 1 &&
           stopped.discarded_ == queued_task_count * 2 &&
           state.destroyed_.load(std::memory_order_relaxed) == queued_task_count &&
           state.rejected_.load(std::memory_order_relaxed) == queued_task_count &&
           state.ran_.load(std::memory_order_relaxed) == 0 && finished.running_ == 0 &&
           finished.completed_ == 1;
}

}  // namespace

int main() {
    bool results = false;
    bool worker_stays_free = false;
    {
        blocking_pool pool(blocking_pool_options{.thread_count_ = 2});
        asio::io_context io_context;
        ruvia::worker_runtime_context runtime(io_context, 8);
        const auto worker_value = runtime.handle();
        ruvia::test::run_worker_tasks(runtime,
            exercise_results(pool, worker_value, results),
            exercise_worker_stays_free(pool, worker_value, worker_stays_free));
        pool.join();
        runtime.detach();
    }

    bool cancelled = false;
    std::atomic_bool cancelled_callable_finished{false};
    {
        thread_gate gate;
        ruvia::stop_source source;
        blocking_pool pool(blocking_pool_options{.thread_count_ = 1});
        asio::io_context io_context;
        ruvia::worker_runtime_context runtime(io_context, 8);
        const auto worker_value = runtime.handle();
        std::thread canceller([&] {
            gate.started_.acquire();
            source.request_stop();
        });
        ruvia::test::run_worker_tasks(runtime, exercise_cancellation(
                                                   pool, worker_value, gate, source, cancelled_callable_finished, cancelled));
        canceller.join();
        pool.join();
        runtime.detach();
    }
    cancelled = cancelled && cancelled_callable_finished.load(std::memory_order_acquire);

    bool timeout = false;
    {
        thread_gate gate;
        blocking_pool pool(blocking_pool_options{.thread_count_ = 2});
        asio::io_context io_context;
        ruvia::worker_runtime_context runtime(io_context, 8);
        const auto worker_value = runtime.handle();
        ruvia::test::run_worker_tasks(runtime, exercise_timeout(pool, worker_value, gate, timeout));
        pool.join();
        runtime.detach();
    }

    bool saturating_timeout = false;
    {
        thread_gate gate;
        blocking_pool pool(blocking_pool_options{.thread_count_ = 1});
        gate.occupy(pool);
        asio::io_context io_context;
        ruvia::worker_runtime_context runtime(io_context, 8);
        const auto worker_value = runtime.handle();
        ruvia::test::run_worker_tasks(runtime,
            exercise_saturating_timeout(pool, worker_value, gate, saturating_timeout));
        pool.join();
        runtime.detach();
    }

    bool stopped_pool = false;
    {
        thread_gate gate;
        blocking_pool pool(blocking_pool_options{.thread_count_ = 1});
        gate.occupy(pool);
        asio::io_context io_context;
        ruvia::worker_runtime_context runtime(io_context, 8);
        const auto worker_value = runtime.handle();
        ruvia::test::run_worker_tasks(runtime,
            exercise_stopped_pool(pool, worker_value, gate, stopped_pool));
        pool.join();
        runtime.detach();
    }

    bool queue_full = false;
    {
        thread_gate gate;
        blocking_pool pool(blocking_pool_options{.thread_count_ = 1, .queue_capacity_ = 1});
        gate.occupy(pool);
        asio::io_context io_context;
        ruvia::worker_runtime_context runtime(io_context, 8);
        const auto worker_value = runtime.handle();
        ruvia::test::run_worker_tasks(runtime, exercise_queue_full(pool, worker_value, gate, queue_full));
        pool.join();
        runtime.detach();
    }

    bool worker_stopping = false;
    {
        thread_gate gate;
        blocking_pool pool(blocking_pool_options{.thread_count_ = 1});
        asio::io_context io_context;
        ruvia::worker_runtime_context runtime(io_context, 8);
        const auto worker_value = runtime.handle();
        ruvia::test::run_worker_tasks(runtime,
            exercise_worker_stopping(runtime, pool, worker_value, gate, worker_stopping));
        gate.release_.release();
        pool.join();
        runtime.detach();
    }

    bool stopped_worker = false;
    std::atomic_bool stopped_worker_task_ran{false};
    {
        blocking_pool pool(blocking_pool_options{.thread_count_ = 1});
        asio::io_context io_context;
        ruvia::worker_runtime_context runtime(io_context, 8);
        const auto worker_value = runtime.handle();
        runtime.close();
        ruvia::test::run_worker_tasks(runtime,
            exercise_stopped_worker(pool, worker_value, stopped_worker_task_ran, stopped_worker));
        pool.join();
        runtime.detach();
        stopped_worker =
            stopped_worker && !stopped_worker_task_ran.load() && pool.stats().completed_ == 0;
    }

    bool rejects_empty_task = false;
    {
        blocking_pool pool(blocking_pool_options{.thread_count_ = 1});
        try {
            static_cast<void>(pool.submit({}));
        } catch (const std::invalid_argument&) {
            rejects_empty_task = true;
        }
        pool.stop();
        pool.join();
        rejects_empty_task =
            rejects_empty_task && pool.submit([] {}) == blocking_submit_status::pool_stopped;
    }

    const bool default_sizing = blocking_pool_defaults_are_bounded_by_cpu_policy();
    const bool destruction_does_not_join = test_destruction_does_not_join_running_callable();
    const bool join_stops_and_waits = test_join_stops_and_waits_for_running_callable();
    const bool join_rejects_pool_thread = test_join_rejects_pool_thread_before_stopping();
    const bool stop_drains_without_running = test_stop_drains_without_running_queued_tasks();
    const bool all_passed = default_sizing && results && worker_stays_free &&
                            cancelled && timeout && saturating_timeout && stopped_pool && queue_full &&
                            worker_stopping && stopped_worker && rejects_empty_task &&
                            destruction_does_not_join && join_stops_and_waits && join_rejects_pool_thread &&
                            stop_drains_without_running;
    return all_passed ? 0 : 1;
}
