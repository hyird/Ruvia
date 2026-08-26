#include "test_harness.h"

#include <cstdint>

#include "ruvia/http/detail/http2/stream/Http2ClosedStreams.h"

namespace {

using ruvia::detail::Http2ClosedStreamHistory;
using ruvia::detail::Http2LocalSettings;
using ruvia::detail::Http2StreamCloseSource;

}  // namespace

RUVIA_TEST(closed_streams_remember_and_update) {
    Http2ClosedStreamHistory history;
    RUVIA_CHECK(!history.source(5).has_value());  // never remembered
    history.remember(5, Http2StreamCloseSource::kLocal);
    RUVIA_CHECK(history.source(5) == Http2StreamCloseSource::kLocal);
    // Remembering the same stream updates its source rather than duplicating it.
    history.remember(5, Http2StreamCloseSource::kPeer);
    RUVIA_CHECK(history.source(5) == Http2StreamCloseSource::kPeer);
}

RUVIA_TEST(closed_streams_ignores_zero_and_invalid_source) {
    Http2ClosedStreamHistory history;
    history.remember(0, Http2StreamCloseSource::kLocal);  // stream 0 is not a real stream
    RUVIA_CHECK(!history.source(0).has_value());
    history.remember(7, static_cast<Http2StreamCloseSource>(0xFF));
    RUVIA_CHECK(!history.source(7).has_value());
}

RUVIA_TEST(closed_streams_evict_oldest_when_full) {
    Http2ClosedStreamHistory history;
    const std::uint32_t limit = Http2LocalSettings::kMaxConcurrentStreams * 4;  // kRecordLimit
    for (std::uint32_t id = 1; id <= limit; ++id) {
        history.remember(id, Http2StreamCloseSource::kLocal);
    }
    RUVIA_CHECK(history.source(1) == Http2StreamCloseSource::kLocal);  // still tracked
    RUVIA_CHECK(history.source(limit) == Http2StreamCloseSource::kLocal);
    // One past the limit evicts the oldest record (id 1) but keeps the rest.
    history.remember(limit + 1, Http2StreamCloseSource::kPeer);
    RUVIA_CHECK(!history.source(1).has_value());                       // evicted
    RUVIA_CHECK(history.source(2) == Http2StreamCloseSource::kLocal);  // retained
    RUVIA_CHECK(history.source(limit + 1) == Http2StreamCloseSource::kPeer);
}

RUVIA_TEST(closed_streams_eviction_survives_ring_buffer_wraparound) {
    // The single-eviction test only advances the replace cursor from 0 to 1. Under
    // sustained reset flooding (the HTTP/2 rapid-reset scenario) the ring buffer
    // wraps many times via `% kRecordLimit`, so an off-by-one in the wrap would
    // corrupt closed-stream tracking under exactly that attack. Insert twice the
    // capacity and confirm FIFO eviction holds across the wrap: the first `limit`
    // ids are all gone, the most recent `limit` are all kept.
    Http2ClosedStreamHistory history;
    const std::uint32_t limit = Http2LocalSettings::kMaxConcurrentStreams * 4;  // kRecordLimit
    for (std::uint32_t id = 1; id <= 2 * limit; ++id) {
        history.remember(id, Http2StreamCloseSource::kLocal);
    }
    RUVIA_CHECK(!history.source(1).has_value());      // long evicted
    RUVIA_CHECK(!history.source(limit).has_value());  // evicted at the wrap
    RUVIA_CHECK(
        history.source(limit + 1) == Http2StreamCloseSource::kLocal);  // oldest still-tracked
    RUVIA_CHECK(history.source(2 * limit) == Http2StreamCloseSource::kLocal);  // newest

    // One further insert evicts exactly the current oldest (limit+1), proving the
    // wrapped cursor still points at the true FIFO front rather than a stale slot.
    history.remember(2 * limit + 1, Http2StreamCloseSource::kPeer);
    RUVIA_CHECK(!history.source(limit + 1).has_value());                       // evicted post-wrap
    RUVIA_CHECK(history.source(limit + 2) == Http2StreamCloseSource::kLocal);  // still tracked
    RUVIA_CHECK(history.source(2 * limit + 1) == Http2StreamCloseSource::kPeer);
}
