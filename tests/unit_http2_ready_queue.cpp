#include "test_harness.h"

#include <cstdint>

#include "net/http2/Http2ReadyQueue.h"

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
