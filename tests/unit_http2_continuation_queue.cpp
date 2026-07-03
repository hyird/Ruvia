#include "test_harness.h"

#include <coroutine>
#include <cstddef>
#include <vector>

#include "net/http2/Http2ContinuationQueue.h"

namespace {

using ruvia::detail::Http2ContinuationQueue;

// A minimal test coroutine: it starts suspended and, when resumed, records its
// id (and optionally pushes a second-generation task back into the queue to
// exercise re-entrant scheduling). The frame stays alive at final suspend so
// the test owns and destroys every handle explicitly.
struct TestPromise;
using TestHandle = std::coroutine_handle<TestPromise>;

struct TestTask {
    using promise_type = TestPromise;
    TestHandle handle;
};

struct TestPromise {
    TestTask get_return_object() noexcept {
        return TestTask{TestHandle::from_promise(*this)};
    }
    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
    void unhandled_exception() noexcept {}
};

TestTask plainTask(std::vector<int>& log, int id) {
    log.push_back(id);
    co_return;
}

TestTask spawningTask(
    Http2ContinuationQueue& queue,
    std::vector<int>& log,
    std::vector<std::coroutine_handle<>>& owned,
    int id,
    int childId) {
    log.push_back(id);
    auto child = plainTask(log, childId);
    owned.push_back(child.handle);
    queue.push(child.handle);
    co_return;
}

void destroyAll(std::vector<std::coroutine_handle<>>& owned) {
    for (auto handle : owned) {
        handle.destroy();
    }
}

}  // namespace

RUVIA_TEST(continuation_queue_inline_fifo_order) {
    Http2ContinuationQueue queue(nullptr);
    std::vector<int> log;
    std::vector<std::coroutine_handle<>> owned;
    for (int i = 0; i < 3; ++i) {
        auto task = plainTask(log, i);
        owned.push_back(task.handle);
        queue.push(task.handle);
    }
    queue.resumeAll();
    RUVIA_CHECK_EQ(log.size(), std::size_t{3});
    RUVIA_CHECK_EQ(log[0], 0);
    RUVIA_CHECK_EQ(log[1], 1);
    RUVIA_CHECK_EQ(log[2], 2);
    destroyAll(owned);
}

RUVIA_TEST(continuation_queue_spill_to_overflow_preserves_fifo) {
    // 16-slot inline capacity; ids 16..19 land in the PMR overflow vector and
    // must still resume after the inline items, in order.
    Http2ContinuationQueue queue(nullptr);
    std::vector<int> log;
    std::vector<std::coroutine_handle<>> owned;
    for (int i = 0; i < 20; ++i) {
        auto task = plainTask(log, i);
        owned.push_back(task.handle);
        queue.push(task.handle);
    }
    queue.resumeAll();
    RUVIA_CHECK_EQ(log.size(), std::size_t{20});
    for (int i = 0; i < 20; ++i) {
        RUVIA_CHECK_EQ(log[static_cast<std::size_t>(i)], i);
    }
    destroyAll(owned);
}

RUVIA_TEST(continuation_queue_interleaved_pop_and_push_stay_fifo) {
    // Drain part of the queue (forcing inline compaction), enqueue more, then
    // drain the rest: overall order must remain strictly FIFO.
    Http2ContinuationQueue queue(nullptr);
    std::vector<int> log;
    std::vector<std::coroutine_handle<>> owned;
    for (int i = 0; i < 10; ++i) {
        auto task = plainTask(log, i);
        owned.push_back(task.handle);
        queue.push(task.handle);
    }
    for (int i = 0; i < 5; ++i) {
        queue.resumeNext();
    }
    RUVIA_CHECK_EQ(log.size(), std::size_t{5});
    for (int i = 10; i < 20; ++i) {
        auto task = plainTask(log, i);
        owned.push_back(task.handle);
        queue.push(task.handle);
    }
    queue.resumeAll();
    RUVIA_CHECK_EQ(log.size(), std::size_t{20});
    for (int i = 0; i < 20; ++i) {
        RUVIA_CHECK_EQ(log[static_cast<std::size_t>(i)], i);
    }
    destroyAll(owned);
}

RUVIA_TEST(continuation_queue_resume_all_current_defers_reentrant_push) {
    // resumeAllCurrent snapshots the current length: tasks pushed by resumed
    // coroutines are deferred to a later pass, not resumed in this one.
    Http2ContinuationQueue queue(nullptr);
    std::vector<int> log;
    std::vector<std::coroutine_handle<>> owned;
    for (int i = 0; i < 3; ++i) {
        auto task = spawningTask(queue, log, owned, i, 100 + i);
        owned.push_back(task.handle);
        queue.push(task.handle);
    }
    queue.resumeAllCurrent();
    // Only the first generation ran; the re-entrant children are still queued.
    RUVIA_CHECK_EQ(log.size(), std::size_t{3});
    RUVIA_CHECK_EQ(log[0], 0);
    RUVIA_CHECK_EQ(log[1], 1);
    RUVIA_CHECK_EQ(log[2], 2);
    queue.resumeAll();
    RUVIA_CHECK_EQ(log.size(), std::size_t{6});
    RUVIA_CHECK_EQ(log[3], 100);
    RUVIA_CHECK_EQ(log[4], 101);
    RUVIA_CHECK_EQ(log[5], 102);
    destroyAll(owned);
}
