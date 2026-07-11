#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/RouteModes.h"
#include "ruvia/web/detail/http2/Http2SansIoStreamRuntime.h"

namespace {

using ruvia::detail::Http2RequestBodyRuntime;
using ruvia::detail::Http2RequestBodyStoreResult;
using ruvia::detail::Http2SansIoBodyQueue;
using ruvia::detail::Http2SansIoStreamRuntimeTable;
using ruvia::detail::RequestBodyMode;

}  // namespace

RUVIA_TEST(http2_web_body_queue_preserves_fifo_and_tracks_backlog) {
    Http2SansIoBodyQueue queue(std::pmr::get_default_resource());
    RUVIA_CHECK(queue.empty());
    queue.enqueue("first");
    queue.enqueue("second");
    queue.enqueue("third");
    RUVIA_CHECK_EQ(queue.queuedBytes(), std::size_t{16});
    const auto active = queue.pop();
    RUVIA_CHECK_EQ(active, std::string_view("first"));
    RUVIA_CHECK_EQ(queue.queuedBytes(), std::size_t{11});
    queue.enqueue("fourth");
    RUVIA_CHECK_EQ(active, std::string_view("first"));
    RUVIA_CHECK_EQ(queue.pop(), std::string_view("second"));
    RUVIA_CHECK_EQ(queue.pop(), std::string_view("third"));
    RUVIA_CHECK_EQ(queue.pop(), std::string_view("fourth"));
    RUVIA_CHECK(queue.empty());
    RUVIA_CHECK_EQ(queue.queuedBytes(), std::size_t{0});
}

RUVIA_TEST(http2_web_body_queue_reuses_storage_and_ignores_empty_chunks) {
    Http2SansIoBodyQueue queue(std::pmr::get_default_resource());
    queue.enqueue({});
    RUVIA_CHECK(queue.empty());
    for (int i = 0; i < 50; ++i) {
        queue.enqueue(std::to_string(i));
    }
    for (int i = 0; i < 50; ++i) {
        const auto expected = std::to_string(i);
        RUVIA_CHECK_EQ(queue.pop(), std::string_view(expected));
    }
    RUVIA_CHECK(queue.empty());
    queue.enqueue("reused");
    RUVIA_CHECK_EQ(queue.pop(), std::string_view("reused"));
}

RUVIA_TEST(http2_web_request_body_runtime_selects_storage_before_data) {
    Http2RequestBodyRuntime buffered(std::pmr::get_default_resource());
    RUVIA_CHECK(buffered.mode() == RequestBodyMode::kBuffered);
    RUVIA_CHECK(buffered.store("rejected", 16, 0) ==
        Http2RequestBodyStoreResult::kModeNotSelected);
    RUVIA_CHECK(buffered.selectMode(RequestBodyMode::kBuffered));
    RUVIA_CHECK(buffered.selectMode(RequestBodyMode::kBuffered));
    RUVIA_CHECK(!buffered.selectMode(RequestBodyMode::kStream));
    RUVIA_CHECK(buffered.store("abc", 3, 0) ==
        Http2RequestBodyStoreResult::kAccepted);
    RUVIA_CHECK_EQ(buffered.buffered(), std::string_view("abc"));
    RUVIA_CHECK_EQ(buffered.receivedBytes(), std::size_t{3});
    RUVIA_CHECK(!buffered.selectMode(RequestBodyMode::kStream));

    Http2RequestBodyRuntime streaming(std::pmr::get_default_resource());
    RUVIA_CHECK(streaming.selectMode(RequestBodyMode::kStream));
    RUVIA_CHECK(streaming.streaming());
    RUVIA_CHECK(streaming.store("one", 0, 8) ==
        Http2RequestBodyStoreResult::kAccepted);
    RUVIA_CHECK(streaming.store("two", 0, 8) ==
        Http2RequestBodyStoreResult::kAccepted);
    RUVIA_CHECK(streaming.buffered().empty());
    RUVIA_CHECK_EQ(streaming.queue().queuedBytes(), std::size_t{6});
}

RUVIA_TEST(http2_web_request_body_runtime_enforces_total_and_backlog_limits) {
    Http2RequestBodyRuntime buffered(std::pmr::get_default_resource());
    RUVIA_CHECK(buffered.selectMode(RequestBodyMode::kBuffered));
    RUVIA_CHECK(buffered.store("1234", 5, 0) ==
        Http2RequestBodyStoreResult::kAccepted);
    RUVIA_CHECK(buffered.store("67", 5, 0) ==
        Http2RequestBodyStoreResult::kTotalLimitExceeded);
    RUVIA_CHECK_EQ(buffered.receivedBytes(), std::size_t{4});
    RUVIA_CHECK_EQ(buffered.buffered(), std::string_view("1234"));

    Http2RequestBodyRuntime streaming(std::pmr::get_default_resource());
    RUVIA_CHECK(streaming.selectMode(RequestBodyMode::kStream));
    RUVIA_CHECK(streaming.store("1234", 0, 5) ==
        Http2RequestBodyStoreResult::kAccepted);
    RUVIA_CHECK(streaming.store("67", 0, 5) ==
        Http2RequestBodyStoreResult::kBacklogLimitExceeded);
    RUVIA_CHECK_EQ(streaming.receivedBytes(), std::size_t{4});
    RUVIA_CHECK_EQ(streaming.queue().pop(), std::string_view("1234"));
    RUVIA_CHECK(streaming.store("67", 0, 5) ==
        Http2RequestBodyStoreResult::kAccepted);
}

RUVIA_TEST(http2_web_stream_runtime_table_keeps_active_storage_stable) {
    std::pmr::monotonic_buffer_resource resource;
    Http2SansIoStreamRuntimeTable table(&resource);
    auto* first = table.ensure(1);
    RUVIA_CHECK(first != nullptr);
    if (first == nullptr) {
        return;
    }
    RUVIA_CHECK(first->body().selectMode(RequestBodyMode::kBuffered));
    RUVIA_CHECK(first->body().store("tiny", 16, 0) ==
        Http2RequestBodyStoreResult::kAccepted);
    const auto firstBody = first->body().buffered();
    const auto* firstAddress = first;

    // Cross the inline capacity so pointer-vector growth and later compaction are
    // both exercised without moving active runtime objects.
    for (std::uint32_t id = 3; id < 45; id += 2) {
        RUVIA_CHECK(table.ensure(id) != nullptr);
    }
    RUVIA_CHECK(table.find(1) == firstAddress);
    RUVIA_CHECK_EQ(table.find(1)->body().buffered(), firstBody);
    RUVIA_CHECK(table.remove(19));
    RUVIA_CHECK(table.find(1) == firstAddress);
    RUVIA_CHECK(!table.remove(19));
    RUVIA_CHECK(table.size() == 21);
}
