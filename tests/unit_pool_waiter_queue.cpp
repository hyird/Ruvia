#include "test_harness.h"

#include <array>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <utility>

#include "ruvia/core/detail/PoolWaiterQueue.h"

namespace {

using ruvia::detail::PoolWaiter;
using ruvia::detail::PoolWaiterAcquired;
using ruvia::detail::PoolWaiterClosed;
using ruvia::detail::PoolWaiterQueue;
using ruvia::detail::PoolWaiterResult;
using ruvia::detail::PoolWaiterTimedOut;

using Clock = std::chrono::steady_clock;

// A far-future deadline so a waiter never expires during a resume/close test.
constexpr Clock::time_point kNever = Clock::time_point::max();

template <typename T>
concept HasAnyRvaluePoolWaiterAccessor =
    requires(T&& result) { std::move(result).acquired(); } ||
    requires(T&& result) { std::move(result).timedOut(); } ||
    requires(T&& result) { std::move(result).closed(); };

static_assert(!std::default_initializable<PoolWaiter>);
static_assert(!std::default_initializable<PoolWaiterResult>);
static_assert(!HasAnyRvaluePoolWaiterAccessor<PoolWaiterResult>);
static_assert(std::same_as<
    decltype(std::declval<const PoolWaiterResult&>().acquired()),
    const PoolWaiterAcquired*>);
static_assert(std::same_as<
    decltype(std::declval<const PoolWaiterResult&>().timedOut()),
    const PoolWaiterTimedOut*>);
static_assert(std::same_as<
    decltype(std::declval<const PoolWaiterResult&>().closed()),
    const PoolWaiterClosed*>);
static_assert(std::same_as<
    decltype(std::declval<PoolWaiter&>().await_resume()),
    const PoolWaiterResult&>);
static_assert(std::same_as<
    decltype(&PoolWaiterQueue::closeAll),
    void (PoolWaiterQueue::*)() noexcept>);

class WaiterProbeTask final {
public:
    struct promise_type final {
        [[nodiscard]] WaiterProbeTask get_return_object() noexcept {
            return WaiterProbeTask(
                std::coroutine_handle<promise_type>::from_promise(*this));
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

    WaiterProbeTask(const WaiterProbeTask&) = delete;
    WaiterProbeTask& operator=(const WaiterProbeTask&) = delete;

    ~WaiterProbeTask() {
        handle_.destroy();
    }

    void start() const noexcept {
        handle_.resume();
    }

private:
    explicit WaiterProbeTask(
        std::coroutine_handle<promise_type> handle) noexcept
        : handle_(handle) {}

    std::coroutine_handle<promise_type> handle_;
};

WaiterProbeTask observeWaiterCompletion(
    PoolWaiter& waiter,
    const PoolWaiterResult*& observed) {
    observed = &(co_await waiter);
}

WaiterProbeTask observeWaiterThenTryResumeNext(
    PoolWaiter& waiter,
    PoolWaiterQueue& queue,
    const PoolWaiterResult*& observed,
    bool& resumedAnotherWaiter) {
    observed = &(co_await waiter);
    resumedAnotherWaiter = queue.resumeNext(999);
}

}  // namespace

RUVIA_TEST(pool_waiter_is_its_own_typed_awaiter) {
    PoolWaiterQueue queue;
    PoolWaiter waiter(kNever);
    queue.enqueue(waiter);

    const PoolWaiterResult* observed = nullptr;
    auto probe = observeWaiterCompletion(waiter, observed);
    probe.start();
    RUVIA_CHECK(observed == nullptr);
    RUVIA_CHECK(!waiter.await_ready());

    RUVIA_CHECK(queue.resumeNext(42));
    RUVIA_CHECK(observed == &waiter.await_resume());
    RUVIA_CHECK(observed->acquired() != nullptr);
    RUVIA_CHECK_EQ(observed->acquired()->index(), std::size_t{42});
}

RUVIA_TEST(pool_waiter_queue_fifo_resume) {
    PoolWaiterQueue queue;
    RUVIA_CHECK(queue.empty());

    std::array<PoolWaiter, 2> waiters{
        PoolWaiter(kNever),
        PoolWaiter(kNever)};
    for (auto& waiter : waiters) {
        queue.enqueue(waiter);
    }
    RUVIA_CHECK(!queue.empty());
    RUVIA_CHECK(!waiters[0].await_ready());
    // Re-enqueuing an already-queued waiter is a no-op.
    queue.enqueue(waiters[0]);

    // FIFO: the first waiter gets the first freed slot.
    RUVIA_CHECK(queue.resumeNext(5));
    const auto* firstResult = &waiters[0].await_resume();
    RUVIA_CHECK(waiters[0].await_ready());
    RUVIA_CHECK(&waiters[0].await_resume() == firstResult);
    RUVIA_CHECK(firstResult->acquired() != nullptr);
    RUVIA_CHECK_EQ(firstResult->acquired()->index(), std::size_t{5});
    RUVIA_CHECK(firstResult->timedOut() == nullptr);
    RUVIA_CHECK(firstResult->closed() == nullptr);
    RUVIA_CHECK(!waiters[1].await_ready());

    RUVIA_CHECK(queue.resumeNext(6));
    const auto* secondResult = &waiters[1].await_resume();
    RUVIA_CHECK(secondResult->acquired() != nullptr);
    RUVIA_CHECK_EQ(secondResult->acquired()->index(), std::size_t{6});

    RUVIA_CHECK(queue.empty());
    RUVIA_CHECK(!queue.resumeNext(7));  // an empty queue yields false
}

RUVIA_TEST(pool_waiter_queue_remove_unlinks_middle_and_is_idempotent) {
    PoolWaiterQueue queue;
    std::array<PoolWaiter, 3> waiters{
        PoolWaiter(kNever),
        PoolWaiter(kNever),
        PoolWaiter(kNever)};
    for (auto& waiter : waiters) {
        queue.enqueue(waiter);
    }
    queue.remove(waiters[1]);  // unlink the middle node
    queue.remove(waiters[1]);  // idempotent

    RUVIA_CHECK(queue.resumeNext(10));
    RUVIA_CHECK(waiters[0].await_resume().acquired() != nullptr);
    RUVIA_CHECK_EQ(
        waiters[0].await_resume().acquired()->index(),
        std::size_t{10});
    RUVIA_CHECK(queue.resumeNext(11));
    RUVIA_CHECK(waiters[2].await_resume().acquired() != nullptr);
    RUVIA_CHECK_EQ(
        waiters[2].await_resume().acquired()->index(),
        std::size_t{11});  // w2 follows w0, w1 skipped
    RUVIA_CHECK(!waiters[1].await_ready());  // removed waiter never completes
    RUVIA_CHECK(queue.empty());
}

RUVIA_TEST(pool_waiter_queue_remove_tail_repoints_tail_for_next_enqueue) {
    // Removing the tail must repoint tail_ to its predecessor. Otherwise the next
    // enqueue links the new waiter off the removed (detached) node, so it is never
    // reachable from head_ and never resumed -- a permanently hung pool acquirer.
    PoolWaiterQueue queue;
    std::array<PoolWaiter, 4> waiters{
        PoolWaiter(kNever),
        PoolWaiter(kNever),
        PoolWaiter(kNever),
        PoolWaiter(kNever)};
    for (int i = 0; i < 3; ++i) {
        queue.enqueue(waiters[i]);
    }
    queue.remove(waiters[2]);  // unlink the tail

    // The new waiter must link off the new tail (w1), not the removed w2.
    queue.enqueue(waiters[3]);

    RUVIA_CHECK(queue.resumeNext(20));
    RUVIA_CHECK_EQ(
        waiters[0].await_resume().acquired()->index(),
        std::size_t{20});
    RUVIA_CHECK(queue.resumeNext(21));
    RUVIA_CHECK_EQ(
        waiters[1].await_resume().acquired()->index(),
        std::size_t{21});
    RUVIA_CHECK(queue.resumeNext(22));
    RUVIA_CHECK_EQ(
        waiters[3].await_resume().acquired()->index(),
        std::size_t{22});  // reachable only via a correct new tail
    RUVIA_CHECK(!waiters[2].await_ready());  // removed tail never completes
    RUVIA_CHECK(queue.empty());
}

RUVIA_TEST(pool_waiter_queue_remove_head) {
    PoolWaiterQueue queue;
    std::array<PoolWaiter, 2> waiters{
        PoolWaiter(kNever),
        PoolWaiter(kNever)};
    for (auto& waiter : waiters) {
        queue.enqueue(waiter);
    }
    queue.remove(waiters[0]);  // unlink the head
    RUVIA_CHECK(queue.resumeNext(4));
    RUVIA_CHECK(waiters[1].await_resume().acquired() != nullptr);
    RUVIA_CHECK_EQ(
        waiters[1].await_resume().acquired()->index(),
        std::size_t{4});
    RUVIA_CHECK(!waiters[0].await_ready());
}

RUVIA_TEST(pool_waiter_queue_close_all_wakes_with_closed_result) {
    PoolWaiterQueue queue;
    std::array<PoolWaiter, 2> waiters{
        PoolWaiter(kNever),
        PoolWaiter(kNever)};
    for (auto& waiter : waiters) {
        queue.enqueue(waiter);
    }
    const PoolWaiterResult* observed[2] = {nullptr, nullptr};
    bool resumedAnotherWaiter = true;
    auto firstProbe = observeWaiterThenTryResumeNext(
        waiters[0],
        queue,
        observed[0],
        resumedAnotherWaiter);
    auto secondProbe = observeWaiterCompletion(waiters[1], observed[1]);
    firstProbe.start();
    secondProbe.start();
    queue.closeAll();
    for (std::size_t i = 0; i < waiters.size(); ++i) {
        const auto* result = &waiters[i].await_resume();
        RUVIA_CHECK(observed[i] == result);
        RUVIA_CHECK(result->closed() != nullptr);
        RUVIA_CHECK(result->acquired() == nullptr);
        RUVIA_CHECK(result->timedOut() == nullptr);
    }
    RUVIA_CHECK(!resumedAnotherWaiter);
    RUVIA_CHECK(queue.empty());
}

RUVIA_TEST(pool_waiter_queue_expire_deadlines_is_selective) {
    PoolWaiterQueue queue;
    const auto now = Clock::now();
    const auto past = now - std::chrono::seconds(1);
    const auto future = now + std::chrono::hours(1);

    std::array<PoolWaiter, 2> waiters{
        PoolWaiter(past),
        PoolWaiter(future)};
    queue.enqueue(waiters[0]);
    queue.enqueue(waiters[1]);
    const PoolWaiterResult* expiredObserved = nullptr;
    const PoolWaiterResult* survivorObserved = nullptr;
    auto expiredProbe =
        observeWaiterCompletion(waiters[0], expiredObserved);
    auto survivorProbe =
        observeWaiterCompletion(waiters[1], survivorObserved);
    expiredProbe.start();
    survivorProbe.start();

    queue.expireDeadlines(now);
    // The expired waiter is failed as a timeout.
    RUVIA_CHECK(waiters[0].await_ready());
    RUVIA_CHECK(waiters[0].await_resume().timedOut() != nullptr);
    RUVIA_CHECK(waiters[0].await_resume().acquired() == nullptr);
    RUVIA_CHECK(waiters[0].await_resume().closed() == nullptr);
    RUVIA_CHECK(expiredObserved == &waiters[0].await_resume());
    // The future-deadline waiter survives and can still be served a slot.
    RUVIA_CHECK(!waiters[1].await_ready());
    RUVIA_CHECK(survivorObserved == nullptr);
    RUVIA_CHECK(!queue.empty());
    RUVIA_CHECK(queue.resumeNext(3));
    RUVIA_CHECK(waiters[1].await_resume().acquired() != nullptr);
    RUVIA_CHECK_EQ(
        waiters[1].await_resume().acquired()->index(),
        std::size_t{3});
    RUVIA_CHECK(survivorObserved == &waiters[1].await_resume());
    RUVIA_CHECK(queue.empty());
}

RUVIA_TEST(pool_waiter_queue_expire_deadlines_interleaved_preserves_survivors) {
    // Interleaved expired/surviving waiters: every expired one is failed and removed
    // while the survivors keep their FIFO order and remain servable. Exercises detach-
    // while-traversing across MULTIPLE removals, not just a single head expiry.
    PoolWaiterQueue queue;
    const auto now = Clock::now();
    const auto past = now - std::chrono::seconds(1);
    const auto future = now + std::chrono::hours(1);

    std::array<PoolWaiter, 4> waiters{
        PoolWaiter(past),     // expired
        PoolWaiter(future),   // survives
        PoolWaiter(past),     // expired
        PoolWaiter(future)};  // survives
    for (auto& w : waiters) {
        queue.enqueue(w);
    }

    queue.expireDeadlines(now);

    RUVIA_CHECK(waiters[0].await_resume().timedOut() != nullptr);
    RUVIA_CHECK(waiters[2].await_resume().timedOut() != nullptr);
    RUVIA_CHECK(!waiters[1].await_ready());  // survivors untouched
    RUVIA_CHECK(!waiters[3].await_ready());

    // The survivors keep FIFO order: 1 is served before 3.
    RUVIA_CHECK(queue.resumeNext(10));
    RUVIA_CHECK_EQ(
        waiters[1].await_resume().acquired()->index(),
        std::size_t{10});
    RUVIA_CHECK(queue.resumeNext(11));
    RUVIA_CHECK_EQ(
        waiters[3].await_resume().acquired()->index(),
        std::size_t{11});
    RUVIA_CHECK(queue.empty());
}
