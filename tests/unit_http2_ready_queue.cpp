#include "test_harness.h"

#include <cstdint>

#include "ruvia/http/detail/http2/Http2ReadyQueue.h"

namespace {

using ruvia::detail::Http2ReadyQueue;
using ruvia::detail::kHttp2LocalMaxConcurrentStreams;

}  // namespace

RUVIA_TEST(ready_queue_fifo_order) {
    Http2ReadyQueue queue;
    RUVIA_CHECK(!queue.hasReady());
    RUVIA_CHECK(queue.push(1));
    RUVIA_CHECK(queue.push(3));
    RUVIA_CHECK(queue.push(5));
    RUVIA_CHECK(queue.hasReady());
    RUVIA_CHECK_EQ(queue.pop(), std::uint32_t{1});  // first in, first out
    RUVIA_CHECK_EQ(queue.pop(), std::uint32_t{3});
    RUVIA_CHECK_EQ(queue.pop(), std::uint32_t{5});
    RUVIA_CHECK(!queue.hasReady());
}

RUVIA_TEST(ready_queue_capacity_and_reuse) {
    Http2ReadyQueue queue;
    const std::uint32_t capacity = kHttp2LocalMaxConcurrentStreams;
    for (std::uint32_t id = 1; id <= capacity; ++id) {
        RUVIA_CHECK(queue.push(id));
    }
    RUVIA_CHECK(!queue.push(9999));  // full
    // Popping enough entries compacts the backing array and frees space again.
    for (std::uint32_t i = 0; i < 64; ++i) {
        RUVIA_CHECK_EQ(queue.pop(), i + 1);
    }
    RUVIA_CHECK(queue.push(9999));
}

RUVIA_TEST(ready_queue_push_reclaims_prefix_when_physically_full) {
    Http2ReadyQueue queue;
    const std::uint32_t capacity = kHttp2LocalMaxConcurrentStreams;
    for (std::uint32_t id = 1; id <= capacity; ++id) {
        RUVIA_CHECK(queue.push(id));
    }
    RUVIA_CHECK(!queue.push(9999));  // genuinely full: nothing consumed yet
    // A single pop leaves the buffer physically full (lazy compaction skips a
    // consumed prefix below the threshold), but the queue now holds one fewer
    // live stream. A push must reclaim that slot rather than spuriously fail --
    // otherwise a ready stream would stall under maximum concurrency.
    RUVIA_CHECK_EQ(queue.pop(), std::uint32_t{1});
    RUVIA_CHECK(queue.push(9999));
    // FIFO order survives the in-push compaction: 2..capacity, then the new id.
    for (std::uint32_t id = 2; id <= capacity; ++id) {
        RUVIA_CHECK_EQ(queue.pop(), id);
    }
    RUVIA_CHECK_EQ(queue.pop(), std::uint32_t{9999});
    RUVIA_CHECK(!queue.hasReady());
}

RUVIA_TEST(ready_queue_remove) {
    Http2ReadyQueue queue;
    RUVIA_CHECK(queue.push(1));
    RUVIA_CHECK(queue.push(2));
    RUVIA_CHECK(queue.push(3));
    RUVIA_CHECK(queue.push(2));  // 2 appears twice
    queue.remove(2);             // both occurrences are removed
    RUVIA_CHECK_EQ(queue.pop(), std::uint32_t{1});
    RUVIA_CHECK_EQ(queue.pop(), std::uint32_t{3});
    RUVIA_CHECK(!queue.hasReady());
}

RUVIA_TEST(ready_queue_remove_after_pop_discards_consumed_prefix) {
    // After some pops the queue has a consumed prefix (offset_ > 0). remove() compacts
    // the ACTIVE range to the front and drops that prefix -- an already-popped stream
    // must never resurface, or it would be dispatched twice.
    Http2ReadyQueue queue;
    RUVIA_CHECK(queue.push(1));
    RUVIA_CHECK(queue.push(2));
    RUVIA_CHECK(queue.push(3));
    RUVIA_CHECK(queue.push(4));
    RUVIA_CHECK_EQ(queue.pop(), std::uint32_t{1});  // consumed
    RUVIA_CHECK_EQ(queue.pop(), std::uint32_t{2});  // consumed; offset_ now past 1 and 2

    queue.remove(3);  // remove a live entry while a consumed prefix exists

    RUVIA_CHECK_EQ(queue.pop(), std::uint32_t{4});  // only 4 survives; 1 and 2 do not return
    RUVIA_CHECK(!queue.hasReady());

    // Removing an id that is not present leaves the surviving order intact.
    RUVIA_CHECK(queue.push(5));
    RUVIA_CHECK(queue.push(6));
    queue.remove(99);  // absent
    RUVIA_CHECK_EQ(queue.pop(), std::uint32_t{5});
    RUVIA_CHECK_EQ(queue.pop(), std::uint32_t{6});
    RUVIA_CHECK(!queue.hasReady());
}

RUVIA_TEST(ready_queue_preserves_order_across_compaction) {
    Http2ReadyQueue queue;
    for (std::uint32_t id = 1; id <= 70; ++id) {
        RUVIA_CHECK(queue.push(id));
    }
    // Popping past offset 64 triggers the memmove compaction path.
    for (std::uint32_t id = 1; id <= 65; ++id) {
        RUVIA_CHECK_EQ(queue.pop(), id);
    }
    // The remaining entries keep their FIFO order after the compaction.
    for (std::uint32_t id = 66; id <= 70; ++id) {
        RUVIA_CHECK_EQ(queue.pop(), id);
    }
    RUVIA_CHECK(!queue.hasReady());
}
