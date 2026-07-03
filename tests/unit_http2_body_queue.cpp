#include "test_harness.h"

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
