#include "test_harness.h"

#include <cstdint>
#include <limits>
#include <memory_resource>
#include <vector>

#include "ruvia/http/detail/http2/Http2StreamTable.h"

namespace {

using ruvia::detail::Http2StreamTable;
using ruvia::detail::kHttp2LocalMaxConcurrentStreams;

}  // namespace

RUVIA_TEST(stream_table_create_find_remove) {
    Http2StreamTable table(std::pmr::get_default_resource());
    RUVIA_CHECK_EQ(table.size(), std::size_t{0});
    RUVIA_CHECK(table.find(1) == nullptr);

    auto* stream = table.create(1, 65535);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK_EQ(table.size(), std::size_t{1});
    RUVIA_CHECK(table.find(1) == stream);
    RUVIA_CHECK_EQ(stream->sendWindow(), std::int32_t{65535});  // seeded with the peer initial window

    // create() is idempotent for an existing stream.
    RUVIA_CHECK(table.create(1, 100) == stream);
    RUVIA_CHECK_EQ(table.size(), std::size_t{1});

    RUVIA_CHECK(table.remove(1));
    RUVIA_CHECK_EQ(table.size(), std::size_t{0});
    RUVIA_CHECK(table.find(1) == nullptr);
    RUVIA_CHECK(!table.remove(1));  // already gone
}

RUVIA_TEST(stream_table_enforces_max_concurrent) {
    Http2StreamTable table(std::pmr::get_default_resource());
    const std::uint32_t limit = kHttp2LocalMaxConcurrentStreams;
    for (std::uint32_t id = 1; id <= limit; ++id) {
        RUVIA_CHECK(table.create(id, 65535) != nullptr);
    }
    RUVIA_CHECK_EQ(table.size(), static_cast<std::size_t>(limit));
    // One beyond the limit is refused (RFC 7540 5.1.2).
    RUVIA_CHECK(table.create(limit + 1, 65535) == nullptr);
    // Removing a stream frees a slot for a new one.
    RUVIA_CHECK(table.remove(1));
    RUVIA_CHECK(table.create(limit + 1, 65535) != nullptr);
    RUVIA_CHECK_EQ(table.size(), static_cast<std::size_t>(limit));
}

RUVIA_TEST(stream_table_apply_send_window_delta) {
    Http2StreamTable table(std::pmr::get_default_resource());
    RUVIA_CHECK(table.create(1, 100) != nullptr);
    RUVIA_CHECK(table.create(3, 200) != nullptr);
    // A SETTINGS_INITIAL_WINDOW_SIZE change adjusts every active stream (RFC 7540 6.9.2).
    RUVIA_CHECK(table.applySendWindowDelta(50));
    RUVIA_CHECK_EQ(table.find(1)->sendWindow(), std::int32_t{150});
    RUVIA_CHECK_EQ(table.find(3)->sendWindow(), std::int32_t{250});
    // A delta that pushes any stream window past 2^31-1 is rejected.
    table.find(1)->setSendWindow(std::numeric_limits<std::int32_t>::max());
    RUVIA_CHECK(!table.applySendWindowDelta(1));
}

RUVIA_TEST(stream_table_apply_send_window_delta_reaches_overflow_streams) {
    Http2StreamTable table(std::pmr::get_default_resource());
    // Spill into overflow storage (kInlineCapacity == 16).
    for (std::uint32_t id = 1; id <= 20; ++id) {
        RUVIA_CHECK(table.create(id, 1000) != nullptr);
    }
    // A SETTINGS_INITIAL_WINDOW_SIZE change must adjust EVERY active stream, including
    // those held in the overflow vector -- missing them would desync flow control for
    // exactly the streams opened after the inline slots filled (RFC 7540 6.9.2).
    RUVIA_CHECK(table.applySendWindowDelta(100));
    RUVIA_CHECK_EQ(table.find(1)->sendWindow(), std::int32_t{1100});   // first inline
    RUVIA_CHECK_EQ(table.find(16)->sendWindow(), std::int32_t{1100});  // last inline
    RUVIA_CHECK_EQ(table.find(17)->sendWindow(), std::int32_t{1100});  // first overflow
    RUVIA_CHECK_EQ(table.find(20)->sendWindow(), std::int32_t{1100});  // deep in overflow
}

RUVIA_TEST(http2_idle_stream_detection) {
    using ruvia::detail::http2IsIdleStream;
    // A stream id higher than any seen has never been opened -> idle.
    RUVIA_CHECK(http2IsIdleStream(7, 5));
    RUVIA_CHECK(http2IsIdleStream(6, 5));
    RUVIA_CHECK(http2IsIdleStream(7, 6));
    // An odd id at or below the highest seen is active/closed -> not idle.
    RUVIA_CHECK(!http2IsIdleStream(5, 5));
    RUVIA_CHECK(!http2IsIdleStream(3, 5));
    // Any even id is idle: client-initiated streams are odd (RFC 7540 5.1.1).
    RUVIA_CHECK(http2IsIdleStream(2, 5));
    RUVIA_CHECK(http2IsIdleStream(4, 5));
}

RUVIA_TEST(stream_table_remove_reset_drops_reset_streams_across_storage) {
    Http2StreamTable table(std::pmr::get_default_resource());
    // 20 streams: 1..16 inline, 17..20 overflow (kInlineCapacity == 16).
    for (std::uint32_t id = 1; id <= 20; ++id) {
        RUVIA_CHECK(table.create(id, 65535) != nullptr);
    }
    RUVIA_CHECK_EQ(table.size(), std::size_t{20});

    // A reset stream in inline storage, plus TWO CONSECUTIVE reset streams in
    // overflow (18, 19): the in-place overflow erase must not skip the neighbour.
    table.find(2)->markReset();
    table.find(18)->markReset();
    table.find(19)->markReset();

    std::vector<std::uint32_t> removed;
    table.removeReset([&removed](const auto& stream) { removed.push_back(stream.id()); });

    RUVIA_CHECK_EQ(removed.size(), std::size_t{3});   // the callback fired once per reset stream
    RUVIA_CHECK_EQ(table.size(), std::size_t{17});    // 20 - 3

    RUVIA_CHECK(table.find(2) == nullptr);
    RUVIA_CHECK(table.find(18) == nullptr);
    RUVIA_CHECK(table.find(19) == nullptr);           // the consecutive overflow reset was not skipped
    RUVIA_CHECK(table.find(1) != nullptr);
    RUVIA_CHECK(table.find(17) != nullptr);
    RUVIA_CHECK(table.find(20) != nullptr);
}
