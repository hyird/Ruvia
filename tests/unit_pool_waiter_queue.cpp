#include "test_harness.h"

#include <chrono>
#include <cstddef>

#include "core/PoolWaiterQueue.h"

namespace {

using ruvia::detail::PoolWaiter;
using ruvia::detail::PoolWaiterQueue;

using Clock = std::chrono::steady_clock;

// A far-future deadline so a waiter never expires during a resume/close test.
constexpr Clock::time_point kNever = Clock::time_point::max();

}  // namespace

RUVIA_TEST(pool_waiter_queue_fifo_resume) {
    PoolWaiterQueue queue;
    RUVIA_CHECK(queue.empty());

    bool ready[2] = {false, false};
    bool timedOut[2] = {false, false};
    std::size_t index[2] = {999, 999};
    PoolWaiter waiters[2];
    for (int i = 0; i < 2; ++i) {
        waiters[i].bind(ready[i], timedOut[i], index[i], kNever, {});
        queue.enqueue(waiters[i]);
    }
    RUVIA_CHECK(!queue.empty());
    // Re-enqueuing an already-queued waiter is a no-op.
    queue.enqueue(waiters[0]);

    // FIFO: the first waiter gets the first freed slot.
    RUVIA_CHECK(queue.resumeNext(5));
    RUVIA_CHECK(ready[0]);
    RUVIA_CHECK_EQ(index[0], std::size_t{5});
    RUVIA_CHECK(!timedOut[0]);
    RUVIA_CHECK(!ready[1]);

    RUVIA_CHECK(queue.resumeNext(6));
    RUVIA_CHECK(ready[1]);
    RUVIA_CHECK_EQ(index[1], std::size_t{6});

    RUVIA_CHECK(queue.empty());
    RUVIA_CHECK(!queue.resumeNext(7));  // an empty queue yields false
}

RUVIA_TEST(pool_waiter_queue_remove_unlinks_middle_and_is_idempotent) {
    PoolWaiterQueue queue;
    bool ready[3] = {false, false, false};
    bool timedOut[3] = {false, false, false};
    std::size_t index[3] = {0, 0, 0};
    PoolWaiter waiters[3];
    for (int i = 0; i < 3; ++i) {
        waiters[i].bind(ready[i], timedOut[i], index[i], kNever, {});
        queue.enqueue(waiters[i]);
    }
    queue.remove(waiters[1]);  // unlink the middle node
    queue.remove(waiters[1]);  // idempotent

    RUVIA_CHECK(queue.resumeNext(10));
    RUVIA_CHECK_EQ(index[0], std::size_t{10});
    RUVIA_CHECK(queue.resumeNext(11));
    RUVIA_CHECK_EQ(index[2], std::size_t{11});  // w2 follows w0, w1 skipped
    RUVIA_CHECK(!ready[1]);                      // the removed waiter is never resumed
    RUVIA_CHECK(queue.empty());
}

RUVIA_TEST(pool_waiter_queue_remove_head) {
    PoolWaiterQueue queue;
    bool ready[2] = {false, false};
    bool timedOut[2] = {false, false};
    std::size_t index[2] = {0, 0};
    PoolWaiter waiters[2];
    for (int i = 0; i < 2; ++i) {
        waiters[i].bind(ready[i], timedOut[i], index[i], kNever, {});
        queue.enqueue(waiters[i]);
    }
    queue.remove(waiters[0]);  // unlink the head
    RUVIA_CHECK(queue.resumeNext(4));
    RUVIA_CHECK(ready[1]);
    RUVIA_CHECK_EQ(index[1], std::size_t{4});
    RUVIA_CHECK(!ready[0]);
}

RUVIA_TEST(pool_waiter_queue_close_all_wakes_with_sentinel) {
    PoolWaiterQueue queue;
    bool ready[2] = {false, false};
    bool timedOut[2] = {true, true};  // start true to prove closeAll clears it
    std::size_t index[2] = {0, 0};
    PoolWaiter waiters[2];
    for (int i = 0; i < 2; ++i) {
        waiters[i].bind(ready[i], timedOut[i], index[i], kNever, {});
        queue.enqueue(waiters[i]);
    }
    queue.closeAll(777);
    for (int i = 0; i < 2; ++i) {
        RUVIA_CHECK(ready[i]);
        RUVIA_CHECK_EQ(index[i], std::size_t{777});  // sentinel slot
        RUVIA_CHECK(!timedOut[i]);                    // closing is not a timeout
    }
    RUVIA_CHECK(queue.empty());
}

RUVIA_TEST(pool_waiter_queue_expire_deadlines_is_selective) {
    PoolWaiterQueue queue;
    const auto now = Clock::now();
    const auto past = now - std::chrono::seconds(1);
    const auto future = now + std::chrono::hours(1);

    bool ready[2] = {false, false};
    bool timedOut[2] = {false, false};
    std::size_t index[2] = {0, 0};
    PoolWaiter waiters[2];
    waiters[0].bind(ready[0], timedOut[0], index[0], past, {});
    waiters[1].bind(ready[1], timedOut[1], index[1], future, {});
    queue.enqueue(waiters[0]);
    queue.enqueue(waiters[1]);

    queue.expireDeadlines(now);
    // The expired waiter is failed as a timeout.
    RUVIA_CHECK(ready[0]);
    RUVIA_CHECK(timedOut[0]);
    // The future-deadline waiter survives and can still be served a slot.
    RUVIA_CHECK(!ready[1]);
    RUVIA_CHECK(!timedOut[1]);
    RUVIA_CHECK(!queue.empty());
    RUVIA_CHECK(queue.resumeNext(3));
    RUVIA_CHECK(ready[1]);
    RUVIA_CHECK_EQ(index[1], std::size_t{3});
    RUVIA_CHECK(queue.empty());
}
