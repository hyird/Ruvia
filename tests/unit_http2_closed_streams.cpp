#include "test_harness.h"

#include <cstdint>

#include "net/http2/Http2ClosedStreams.h"

namespace {

using ruvia::detail::Http2ClosedStreamHistory;
using ruvia::detail::Http2StreamCloseSource;
using ruvia::detail::kHttp2LocalMaxConcurrentStreams;

}  // namespace

RUVIA_TEST(closed_streams_remember_and_update) {
    Http2ClosedStreamHistory history;
    RUVIA_CHECK(history.source(5) == Http2StreamCloseSource::kNone);  // never remembered
    history.remember(5, Http2StreamCloseSource::kLocal);
    RUVIA_CHECK(history.source(5) == Http2StreamCloseSource::kLocal);
    // Remembering the same stream updates its source rather than duplicating it.
    history.remember(5, Http2StreamCloseSource::kPeer);
    RUVIA_CHECK(history.source(5) == Http2StreamCloseSource::kPeer);
}

RUVIA_TEST(closed_streams_ignores_zero_and_none_source) {
    Http2ClosedStreamHistory history;
    history.remember(0, Http2StreamCloseSource::kLocal);  // stream 0 is not a real stream
    RUVIA_CHECK(history.source(0) == Http2StreamCloseSource::kNone);
    history.remember(7, Http2StreamCloseSource::kNone);   // a kNone source is a no-op
    RUVIA_CHECK(history.source(7) == Http2StreamCloseSource::kNone);
}

RUVIA_TEST(closed_streams_evict_oldest_when_full) {
    Http2ClosedStreamHistory history;
    const std::uint32_t limit = kHttp2LocalMaxConcurrentStreams * 4;  // kRecordLimit
    for (std::uint32_t id = 1; id <= limit; ++id) {
        history.remember(id, Http2StreamCloseSource::kLocal);
    }
    RUVIA_CHECK(history.source(1) == Http2StreamCloseSource::kLocal);      // still tracked
    RUVIA_CHECK(history.source(limit) == Http2StreamCloseSource::kLocal);
    // One past the limit evicts the oldest record (id 1) but keeps the rest.
    history.remember(limit + 1, Http2StreamCloseSource::kPeer);
    RUVIA_CHECK(history.source(1) == Http2StreamCloseSource::kNone);       // evicted
    RUVIA_CHECK(history.source(2) == Http2StreamCloseSource::kLocal);      // retained
    RUVIA_CHECK(history.source(limit + 1) == Http2StreamCloseSource::kPeer);
}
