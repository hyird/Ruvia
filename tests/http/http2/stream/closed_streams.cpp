#include <cstdint>

#include "http2/http2_closed_streams.h"
#include "test_harness.h"

namespace {

using ruvia::detail::http2_closed_stream_history;
using ruvia::detail::http2_local_settings;
using ruvia::detail::http2_stream_close_source;

}  // namespace

RUVIA_TEST(closed_streams_remember_and_update) {
    http2_closed_stream_history history;
    RUVIA_CHECK(!history.source(5).has_value());  // never remembered
    history.remember(5, http2_stream_close_source::local);
    RUVIA_CHECK(history.source(5) == http2_stream_close_source::local);
    // Remembering the same stream updates its source rather than duplicating it.
    history.remember(5, http2_stream_close_source::peer);
    RUVIA_CHECK(history.source(5) == http2_stream_close_source::peer);
}

RUVIA_TEST(closed_streams_ignores_zero_and_invalid_source) {
    http2_closed_stream_history history;
    history.remember(0, http2_stream_close_source::local);  // stream 0 is not a real stream
    RUVIA_CHECK(!history.source(0).has_value());
    history.remember(7, static_cast<http2_stream_close_source>(0xFF));
    RUVIA_CHECK(!history.source(7).has_value());
}

RUVIA_TEST(closed_streams_evict_oldest_when_full) {
    http2_closed_stream_history history;
    const std::uint32_t limit = http2_local_settings::max_concurrent_streams * 4;  // record_limit
    for (std::uint32_t id = 1; id <= limit; ++id) {
        history.remember(id, http2_stream_close_source::local);
    }
    RUVIA_CHECK(history.source(1) == http2_stream_close_source::local);  // still tracked
    RUVIA_CHECK(history.source(limit) == http2_stream_close_source::local);
    // One past the limit evicts the oldest record (id 1) but keeps the rest.
    history.remember(limit + 1, http2_stream_close_source::peer);
    RUVIA_CHECK(!history.source(1).has_value());                         // evicted
    RUVIA_CHECK(history.source(2) == http2_stream_close_source::local);  // retained
    RUVIA_CHECK(history.source(limit + 1) == http2_stream_close_source::peer);
}

RUVIA_TEST(closed_streams_eviction_survives_ring_buffer_wraparound) {
    // The single-eviction test only advances the replace cursor from 0 to 1. Under
    // sustained reset flooding (the HTTP/2 rapid-reset scenario) the ring buffer
    // wraps many times via `% record_limit`, so an off-by-one in the wrap would
    // corrupt closed-stream tracking under exactly that attack. Insert twice the
    // capacity and confirm FIFO eviction holds across the wrap: the first `limit`
    // ids are all gone, the most recent `limit` are all kept.
    http2_closed_stream_history history;
    const std::uint32_t limit = http2_local_settings::max_concurrent_streams * 4;  // record_limit
    for (std::uint32_t id = 1; id <= 2 * limit; ++id) {
        history.remember(id, http2_stream_close_source::local);
    }
    RUVIA_CHECK(!history.source(1).has_value());      // long evicted
    RUVIA_CHECK(!history.source(limit).has_value());  // evicted at the wrap
    RUVIA_CHECK(
        history.source(limit + 1) == http2_stream_close_source::local);          // oldest still-tracked
    RUVIA_CHECK(history.source(2 * limit) == http2_stream_close_source::local);  // newest

    // One further insert evicts exactly the current oldest (limit+1), proving the
    // wrapped cursor still points at the true FIFO front rather than a stale slot.
    history.remember(2 * limit + 1, http2_stream_close_source::peer);
    RUVIA_CHECK(!history.source(limit + 1).has_value());                         // evicted post-wrap
    RUVIA_CHECK(history.source(limit + 2) == http2_stream_close_source::local);  // still tracked
    RUVIA_CHECK(history.source(2 * limit + 1) == http2_stream_close_source::peer);
}
