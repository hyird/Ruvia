#include "test_harness.h"

#include <cstdint>
#include <limits>
#include <memory_resource>

#include "net/http2/Http2StreamTable.h"

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
