#include "test_harness.h"

#include <coroutine>
#include <memory_resource>
#include <string>
#include <string_view>

#include "net/http2/Http2BodyQueue.h"

namespace {

using ruvia::detail::Http2StreamBodyQueue;

}  // namespace

RUVIA_TEST(body_queue_fifo_order) {
    Http2StreamBodyQueue queue(std::pmr::get_default_resource());
    RUVIA_CHECK(!queue.hasQueuedChunk());
    queue.enqueue("first");
    queue.enqueue("second");  // beyond the single fast slot -> overflow vector
    queue.enqueue("third");
    RUVIA_CHECK(queue.hasQueuedChunk());
    RUVIA_CHECK_EQ(queue.pop(), std::string_view("first"));
    RUVIA_CHECK_EQ(queue.pop(), std::string_view("second"));
    RUVIA_CHECK_EQ(queue.pop(), std::string_view("third"));
    RUVIA_CHECK(!queue.hasQueuedChunk());
    RUVIA_CHECK(queue.pop().empty());  // popping an empty queue yields an empty view
}

RUVIA_TEST(body_queue_tracks_queued_bytes) {
    Http2StreamBodyQueue queue(std::pmr::get_default_resource());
    RUVIA_CHECK_EQ(queue.queuedBytes(), std::size_t{0});

    // Fast slot then overflow: the counter sums the un-popped backlog.
    queue.enqueue("first");   // 5, fast slot
    queue.enqueue("second");  // 6, overflow
    queue.enqueue("third");   // 5, overflow
    RUVIA_CHECK_EQ(queue.queuedBytes(), std::size_t{16});

    // Empty chunks do not move the counter.
    queue.enqueue("");
    RUVIA_CHECK_EQ(queue.queuedBytes(), std::size_t{16});

    // Popping drains the counter chunk by chunk (fast slot then overflow tail); the
    // active chunk the reader now holds is not counted.
    RUVIA_CHECK_EQ(queue.pop(), std::string_view("first"));
    RUVIA_CHECK_EQ(queue.queuedBytes(), std::size_t{11});
    RUVIA_CHECK_EQ(queue.pop(), std::string_view("second"));
    RUVIA_CHECK_EQ(queue.queuedBytes(), std::size_t{5});
    RUVIA_CHECK_EQ(queue.pop(), std::string_view("third"));
    RUVIA_CHECK_EQ(queue.queuedBytes(), std::size_t{0});

    // Popping an empty queue leaves the counter at zero (no underflow).
    RUVIA_CHECK(queue.pop().empty());
    RUVIA_CHECK_EQ(queue.queuedBytes(), std::size_t{0});

    // enqueueOwned accounts the moved body too.
    std::pmr::string owned("owned-body", std::pmr::get_default_resource());  // 10
    queue.enqueueOwned(owned);
    RUVIA_CHECK_EQ(queue.queuedBytes(), std::size_t{10});
    RUVIA_CHECK_EQ(queue.pop(), std::string_view("owned-body"));
    RUVIA_CHECK_EQ(queue.queuedBytes(), std::size_t{0});
}

RUVIA_TEST(body_queue_ignores_empty_chunks) {
    Http2StreamBodyQueue queue(std::pmr::get_default_resource());
    queue.enqueue("");
    queue.enqueue(std::string_view());
    RUVIA_CHECK(!queue.hasQueuedChunk());
    // Empty enqueues around a real chunk are still dropped.
    queue.enqueue("payload");
    queue.enqueue("");
    RUVIA_CHECK_EQ(queue.pop(), std::string_view("payload"));
    RUVIA_CHECK(!queue.hasQueuedChunk());
}

RUVIA_TEST(body_queue_many_chunks_through_overflow) {
    Http2StreamBodyQueue queue(std::pmr::get_default_resource());
    constexpr int kCount = 50;
    for (int i = 0; i < kCount; ++i) {
        queue.enqueue(std::to_string(i));
    }
    for (int i = 0; i < kCount; ++i) {
        const std::string expected = std::to_string(i);
        RUVIA_CHECK_EQ(queue.pop(), std::string_view(expected));  // FIFO across the overflow vector
    }
    RUVIA_CHECK(!queue.hasQueuedChunk());
    RUVIA_CHECK(queue.pop().empty());
}

RUVIA_TEST(body_queue_enqueue_owned_moves_and_clears_source) {
    Http2StreamBodyQueue queue(std::pmr::get_default_resource());
    std::pmr::string body("owned-body", std::pmr::get_default_resource());
    queue.enqueueOwned(body);
    RUVIA_CHECK(body.empty());  // ownership transferred; the source is cleared
    RUVIA_CHECK(queue.hasQueuedChunk());
    RUVIA_CHECK_EQ(queue.pop(), std::string_view("owned-body"));

    // A second owned chunk goes to the overflow vector and preserves order.
    std::pmr::string first("aaa", std::pmr::get_default_resource());
    std::pmr::string second("bbb", std::pmr::get_default_resource());
    queue.enqueueOwned(first);
    queue.enqueueOwned(second);
    RUVIA_CHECK(queue.hasOverflowQueuedChunk());
    RUVIA_CHECK_EQ(queue.pop(), std::string_view("aaa"));
    RUVIA_CHECK_EQ(queue.pop(), std::string_view("bbb"));

    // An empty owned body is a no-op and leaves the source untouched.
    std::pmr::string emptyBody(std::pmr::get_default_resource());
    queue.enqueueOwned(emptyBody);
    RUVIA_CHECK(!queue.hasQueuedChunk());
}

RUVIA_TEST(body_queue_hot_slot_reused_after_drain) {
    Http2StreamBodyQueue queue(std::pmr::get_default_resource());
    queue.enqueue("a");
    RUVIA_CHECK_EQ(queue.pop(), std::string_view("a"));
    // Drained: the next chunk takes the fast hot slot again, not the overflow.
    queue.enqueue("b");
    RUVIA_CHECK(!queue.hasOverflowQueuedChunk());
    RUVIA_CHECK_EQ(queue.pop(), std::string_view("b"));
}

RUVIA_TEST(body_queue_enqueue_after_partial_drain_stays_fifo) {
    // The hot slot is a fast path only for a queue that is otherwise empty. Once
    // the overflow vector holds data, a new chunk must go there too -- even after
    // the hot slot is drained and free -- or it would jump ahead of already-queued
    // bytes and REORDER the request body. (hot_slot_reused covers the fully-drained
    // case; this covers a hot-slot-free-but-overflow-non-empty state.)
    Http2StreamBodyQueue queue(std::pmr::get_default_resource());
    queue.enqueue("1");  // -> hot slot
    queue.enqueue("2");  // -> overflow (hot slot occupied)
    RUVIA_CHECK_EQ(queue.pop(), std::string_view("1"));  // drains the hot slot
    RUVIA_CHECK(queue.hasOverflowQueuedChunk());          // "2" still queued in overflow

    queue.enqueue("3");  // hot slot is free, but overflow is non-empty -> must overflow
    RUVIA_CHECK_EQ(queue.pop(), std::string_view("2"));   // not "3"
    RUVIA_CHECK_EQ(queue.pop(), std::string_view("3"));
    RUVIA_CHECK(queue.pop().empty());
}

RUVIA_TEST(body_queue_waiter_set_and_take_is_one_shot) {
    Http2StreamBodyQueue queue(std::pmr::get_default_resource());
    RUVIA_CHECK(!queue.takeWaiter());  // no waiter registered initially
    const auto handle = std::noop_coroutine();
    queue.setWaiter(handle);
    RUVIA_CHECK(queue.takeWaiter() == handle);
    RUVIA_CHECK(!queue.takeWaiter());  // taking clears it — a resume happens once
}
