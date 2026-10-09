#include "pool_waiter_queue.h"

#include <array>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <utility>

#include "test_harness.h"

namespace {

using ruvia::detail::pool_waiter;
using ruvia::detail::pool_waiter_acquired;
using ruvia::detail::pool_waiter_cancelled;
using ruvia::detail::pool_waiter_closed;
using ruvia::detail::pool_waiter_queue;
using ruvia::detail::pool_waiter_result;
using ruvia::detail::pool_waiter_timed_out;

using clock_type = std::chrono::steady_clock;

// A far-future deadline so a waiter never expires during a resume/close test.
constexpr clock_type::time_point never = clock_type::time_point::max();

class waiter_probe_task final {
public:
    struct promise_type final {
        [[nodiscard]] waiter_probe_task get_return_object() noexcept {
            return waiter_probe_task(std::coroutine_handle<promise_type>::from_promise(*this));
        }

        [[nodiscard]] std::suspend_always initial_suspend() const noexcept {
            return {};
        }

        [[nodiscard]] std::suspend_always final_suspend() const noexcept {
            return {};
        }

        void return_void() const noexcept {}

        [[noreturn]] void unhandled_exception() const noexcept {
            std::terminate();
        }
    };

    waiter_probe_task(const waiter_probe_task&) = delete;
    waiter_probe_task& operator=(const waiter_probe_task&) = delete;

    ~waiter_probe_task() {
        handle_.destroy();
    }

    void start() const noexcept {
        handle_.resume();
    }

private:
    explicit waiter_probe_task(std::coroutine_handle<promise_type> handle) noexcept
        : handle_(handle) {}

    std::coroutine_handle<promise_type> handle_;
};

waiter_probe_task observe_waiter_completion(pool_waiter& waiter, const pool_waiter_result*& observed_value) {
    observed_value = &(co_await waiter);
}

waiter_probe_task observe_waiter_then_try_resume_next(pool_waiter& waiter, pool_waiter_queue& queue,
    const pool_waiter_result*& observed_value, bool& resumed_another_waiter) {
    observed_value = &(co_await waiter);
    resumed_another_waiter = queue.resume_next(999);
}

}  // namespace

RUVIA_TEST(pool_waiter_is_its_own_typed_awaiter) {
    pool_waiter_queue queue;
    pool_waiter waiter(never);
    queue.enqueue(waiter);

    const pool_waiter_result* observed_value = nullptr;
    auto probe_value = observe_waiter_completion(waiter, observed_value);
    probe_value.start();
    RUVIA_CHECK(observed_value == nullptr);
    RUVIA_CHECK(!waiter.await_ready());

    RUVIA_CHECK(queue.resume_next(42));
    RUVIA_CHECK(observed_value == &waiter.await_resume());
    RUVIA_CHECK(observed_value->acquired() != nullptr);
    RUVIA_CHECK_EQ(observed_value->acquired()->index(), std::size_t{42});
}

RUVIA_TEST(pool_waiter_queue_fifo_resume) {
    pool_waiter_queue queue;
    RUVIA_CHECK(queue.empty());

    std::array<pool_waiter, 2> waiters{pool_waiter(never), pool_waiter(never)};
    for (auto& waiter : waiters) {
        queue.enqueue(waiter);
    }
    RUVIA_CHECK(!queue.empty());
    RUVIA_CHECK(!waiters[0].await_ready());
    // Re-enqueuing an already-queued waiter is a no-op.
    queue.enqueue(waiters[0]);

    // FIFO: the first waiter gets the first freed slot.
    RUVIA_CHECK(queue.resume_next(5));
    const auto* first_result = &waiters[0].await_resume();
    RUVIA_CHECK(waiters[0].await_ready());
    RUVIA_CHECK(&waiters[0].await_resume() == first_result);
    RUVIA_CHECK(first_result->acquired() != nullptr);
    RUVIA_CHECK_EQ(first_result->acquired()->index(), std::size_t{5});
    RUVIA_CHECK(first_result->timed_out() == nullptr);
    RUVIA_CHECK(first_result->closed() == nullptr);
    RUVIA_CHECK(!waiters[1].await_ready());

    // A completed waiter cannot re-enter the intrusive queue or lose its result.
    queue.enqueue(waiters[0]);
    queue.remove(waiters[0]);
    RUVIA_CHECK(&waiters[0].await_resume() == first_result);

    RUVIA_CHECK(queue.resume_next(6));
    const auto* second_result = &waiters[1].await_resume();
    RUVIA_CHECK(second_result->acquired() != nullptr);
    RUVIA_CHECK_EQ(second_result->acquired()->index(), std::size_t{6});

    RUVIA_CHECK(queue.empty());
    RUVIA_CHECK(!queue.resume_next(7));  // an empty queue yields false
}

RUVIA_TEST(pool_waiter_queue_remove_unlinks_middle_and_is_idempotent) {
    pool_waiter_queue queue;
    std::array<pool_waiter, 3> waiters{pool_waiter(never), pool_waiter(never), pool_waiter(never)};
    for (auto& waiter : waiters) {
        queue.enqueue(waiter);
    }
    queue.remove(waiters[1]);  // unlink the middle node
    queue.remove(waiters[1]);  // idempotent

    RUVIA_CHECK(queue.resume_next(10));
    RUVIA_CHECK(waiters[0].await_resume().acquired() != nullptr);
    RUVIA_CHECK_EQ(waiters[0].await_resume().acquired()->index(), std::size_t{10});
    RUVIA_CHECK(queue.resume_next(11));
    RUVIA_CHECK(waiters[2].await_resume().acquired() != nullptr);
    RUVIA_CHECK_EQ(waiters[2].await_resume().acquired()->index(),
        std::size_t{11});                    // w2 follows w0, w1 skipped
    RUVIA_CHECK(!waiters[1].await_ready());  // removed waiter never completes
    RUVIA_CHECK(queue.empty());
}

RUVIA_TEST(pool_waiter_queue_remove_tail_repoints_tail_for_next_enqueue) {
    // Removing the tail must repoint tail_ to its predecessor. Otherwise the next
    // enqueue links the new waiter off the removed (detached) node, so it is never
    // reachable from head_ and never resumed -- a permanently hung pool acquirer.
    pool_waiter_queue queue;
    std::array<pool_waiter, 4> waiters{
        pool_waiter(never), pool_waiter(never), pool_waiter(never), pool_waiter(never)};
    for (int i = 0; i < 3; ++i) {
        queue.enqueue(waiters[i]);
    }
    queue.remove(waiters[2]);  // unlink the tail

    // The new waiter must link off the new tail (w1), not the removed w2.
    queue.enqueue(waiters[3]);

    RUVIA_CHECK(queue.resume_next(20));
    RUVIA_CHECK_EQ(waiters[0].await_resume().acquired()->index(), std::size_t{20});
    RUVIA_CHECK(queue.resume_next(21));
    RUVIA_CHECK_EQ(waiters[1].await_resume().acquired()->index(), std::size_t{21});
    RUVIA_CHECK(queue.resume_next(22));
    RUVIA_CHECK_EQ(waiters[3].await_resume().acquired()->index(),
        std::size_t{22});                    // reachable only via a correct new tail
    RUVIA_CHECK(!waiters[2].await_ready());  // removed tail never completes
    RUVIA_CHECK(queue.empty());
}

RUVIA_TEST(pool_waiter_queue_remove_head) {
    pool_waiter_queue queue;
    std::array<pool_waiter, 2> waiters{pool_waiter(never), pool_waiter(never)};
    for (auto& waiter : waiters) {
        queue.enqueue(waiter);
    }
    queue.remove(waiters[0]);  // unlink the head
    RUVIA_CHECK(queue.resume_next(4));
    RUVIA_CHECK(waiters[1].await_resume().acquired() != nullptr);
    RUVIA_CHECK_EQ(waiters[1].await_resume().acquired()->index(), std::size_t{4});
    RUVIA_CHECK(!waiters[0].await_ready());
}

RUVIA_TEST(pool_waiter_queue_removed_waiter_can_reenter_from_idle) {
    pool_waiter_queue queue;
    pool_waiter waiter(never);
    queue.enqueue(waiter);
    queue.remove(waiter);
    queue.enqueue(waiter);

    RUVIA_CHECK(queue.resume_next(8));
    RUVIA_CHECK(waiter.await_resume().acquired() != nullptr);
    RUVIA_CHECK_EQ(waiter.await_resume().acquired()->index(), std::size_t{8});
    RUVIA_CHECK(queue.empty());
}

RUVIA_TEST(pool_waiter_queue_close_all_wakes_with_closed_result) {
    pool_waiter_queue queue;
    std::array<pool_waiter, 2> waiters{pool_waiter(never), pool_waiter(never)};
    for (auto& waiter : waiters) {
        queue.enqueue(waiter);
    }
    const pool_waiter_result* observed_value[2] = {nullptr, nullptr};
    bool resumed_another_waiter = true;
    auto first_probe =
        observe_waiter_then_try_resume_next(waiters[0], queue, observed_value[0], resumed_another_waiter);
    auto second_probe = observe_waiter_completion(waiters[1], observed_value[1]);
    first_probe.start();
    second_probe.start();
    queue.close_all();
    for (std::size_t i = 0; i < waiters.size(); ++i) {
        const auto* result_value = &waiters[i].await_resume();
        RUVIA_CHECK(observed_value[i] == result_value);
        RUVIA_CHECK(result_value->closed() != nullptr);
        RUVIA_CHECK(result_value->acquired() == nullptr);
        RUVIA_CHECK(result_value->timed_out() == nullptr);
    }
    RUVIA_CHECK(!resumed_another_waiter);
    RUVIA_CHECK(queue.empty());
}

RUVIA_TEST(pool_waiter_queue_cancel_unlinks_and_wakes_with_cancelled_result) {
    pool_waiter_queue queue;
    pool_waiter waiter(never);
    queue.enqueue(waiter);
    const pool_waiter_result* observed_value = nullptr;
    auto probe_value = observe_waiter_completion(waiter, observed_value);
    probe_value.start();

    RUVIA_CHECK(queue.cancel(waiter));
    RUVIA_CHECK(observed_value == &waiter.await_resume());
    RUVIA_CHECK(observed_value->cancelled() != nullptr);
    RUVIA_CHECK(observed_value->acquired() == nullptr);
    RUVIA_CHECK(observed_value->timed_out() == nullptr);
    RUVIA_CHECK(observed_value->closed() == nullptr);
    RUVIA_CHECK(queue.empty());
    RUVIA_CHECK(!queue.cancel(waiter));
}

RUVIA_TEST(pool_waiter_queue_expire_deadlines_is_selective) {
    pool_waiter_queue queue;
    const auto now = clock_type::now();
    const auto past = now - std::chrono::seconds(1);
    const auto future = now + std::chrono::hours(1);

    std::array<pool_waiter, 2> waiters{pool_waiter(past), pool_waiter(future)};
    queue.enqueue(waiters[0]);
    queue.enqueue(waiters[1]);
    const pool_waiter_result* expired_observed = nullptr;
    const pool_waiter_result* survivor_observed = nullptr;
    auto expired_probe = observe_waiter_completion(waiters[0], expired_observed);
    auto survivor_probe = observe_waiter_completion(waiters[1], survivor_observed);
    expired_probe.start();
    survivor_probe.start();

    queue.expire_deadlines(now);
    // The expired waiter is failed as a timeout.
    RUVIA_CHECK(waiters[0].await_ready());
    RUVIA_CHECK(waiters[0].await_resume().timed_out() != nullptr);
    RUVIA_CHECK(waiters[0].await_resume().acquired() == nullptr);
    RUVIA_CHECK(waiters[0].await_resume().closed() == nullptr);
    RUVIA_CHECK(expired_observed == &waiters[0].await_resume());
    // The future-deadline waiter survives and can still be served a slot.
    RUVIA_CHECK(!waiters[1].await_ready());
    RUVIA_CHECK(survivor_observed == nullptr);
    RUVIA_CHECK(!queue.empty());
    RUVIA_CHECK(queue.resume_next(3));
    RUVIA_CHECK(waiters[1].await_resume().acquired() != nullptr);
    RUVIA_CHECK_EQ(waiters[1].await_resume().acquired()->index(), std::size_t{3});
    RUVIA_CHECK(survivor_observed == &waiters[1].await_resume());
    RUVIA_CHECK(queue.empty());
}

RUVIA_TEST(pool_waiter_queue_expire_deadlines_interleaved_preserves_survivors) {
    // Interleaved expired/surviving waiters: every expired one is failed and removed
    // while the survivors keep their FIFO order and remain servable. Exercises detach-
    // while-traversing across MULTIPLE removals, not just a single head expiry.
    pool_waiter_queue queue;
    const auto now = clock_type::now();
    const auto past = now - std::chrono::seconds(1);
    const auto future = now + std::chrono::hours(1);

    std::array<pool_waiter, 4> waiters{pool_waiter(past),  // expired
        pool_waiter(future),                               // survives
        pool_waiter(past),                                 // expired
        pool_waiter(future)};                              // survives
    for (auto& w : waiters) {
        queue.enqueue(w);
    }

    queue.expire_deadlines(now);

    RUVIA_CHECK(waiters[0].await_resume().timed_out() != nullptr);
    RUVIA_CHECK(waiters[2].await_resume().timed_out() != nullptr);
    RUVIA_CHECK(!waiters[1].await_ready());  // survivors untouched
    RUVIA_CHECK(!waiters[3].await_ready());

    // The survivors keep FIFO order: 1 is served before 3.
    RUVIA_CHECK(queue.resume_next(10));
    RUVIA_CHECK_EQ(waiters[1].await_resume().acquired()->index(), std::size_t{10});
    RUVIA_CHECK(queue.resume_next(11));
    RUVIA_CHECK_EQ(waiters[3].await_resume().acquired()->index(), std::size_t{11});
    RUVIA_CHECK(queue.empty());
}
