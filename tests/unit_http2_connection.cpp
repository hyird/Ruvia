#include "test_harness.h"

#include <cstdint>
#include <memory_resource>
#include <string_view>

#include "net/http2/Http2Connection.h"
#include "net/http2/Http2FrameCodec.h"

namespace {

using ruvia::detail::Http2Connection;
using ruvia::detail::Http2FrameType;

}  // namespace

// The sans-I/O core produces a SETTINGS frame (stream 0) into its outbound buffer,
// and consumeOutput drains it. Exercises the core with zero asio / zero I/O.
RUVIA_TEST(http2_connection_queue_local_settings_emits_settings_frame) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);

    RUVIA_CHECK(conn.pendingOutput().empty());
    RUVIA_CHECK(!conn.wantsWrite());

    conn.queueLocalSettings();

    const auto out = conn.pendingOutput();
    RUVIA_CHECK(out.size() >= 9);  // at least one frame header
    RUVIA_CHECK(conn.wantsWrite());

    const auto header = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(header.type, static_cast<std::uint8_t>(Http2FrameType::kSettings));
    RUVIA_CHECK_EQ(header.streamId, static_cast<std::uint32_t>(0));

    conn.consumeOutput(out.size());
    RUVIA_CHECK(conn.pendingOutput().empty());
    RUVIA_CHECK(!conn.wantsWrite());
}
