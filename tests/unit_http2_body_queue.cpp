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

RUVIA_TEST(body_queue_waiter_set_and_take_is_one_shot) {
    Http2StreamBodyQueue queue(std::pmr::get_default_resource());
    RUVIA_CHECK(!queue.takeWaiter());  // no waiter registered initially
    const auto handle = std::noop_coroutine();
    queue.setWaiter(handle);
    RUVIA_CHECK(queue.takeWaiter() == handle);
    RUVIA_CHECK(!queue.takeWaiter());  // taking clears it — a resume happens once
}
