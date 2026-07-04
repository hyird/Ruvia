#include "test_harness.h"

#include "net/http2/Http2StreamLifecycle.h"

namespace {

using ruvia::detail::Http2StreamCloseSource;
using ruvia::detail::Http2StreamLifecycle;

}  // namespace

RUVIA_TEST(stream_lifecycle_initial_and_reset_source) {
    Http2StreamLifecycle lifecycle;
    RUVIA_CHECK(!lifecycle.reset());
    RUVIA_CHECK(!lifecycle.bodyEnded());
    RUVIA_CHECK(!lifecycle.peerEndStream());
    RUVIA_CHECK(!lifecycle.queued());
    RUVIA_CHECK(!lifecycle.dispatchStarted());
    RUVIA_CHECK(lifecycle.closeSource() == Http2StreamCloseSource::kNone);

    lifecycle.markReset(Http2StreamCloseSource::kLocal);
    RUVIA_CHECK(lifecycle.reset());
    RUVIA_CHECK(lifecycle.closeSource() == Http2StreamCloseSource::kLocal);
    // A subsequent kNone reset does not overwrite the recorded source.
    lifecycle.markReset(Http2StreamCloseSource::kNone);
    RUVIA_CHECK(lifecycle.closeSource() == Http2StreamCloseSource::kLocal);
}

RUVIA_TEST(stream_lifecycle_mark_closed_sets_all_end_flags) {
    Http2StreamLifecycle lifecycle;
    lifecycle.markClosed(Http2StreamCloseSource::kPeer);
    RUVIA_CHECK(lifecycle.reset());
    RUVIA_CHECK(lifecycle.peerEndStream());
    RUVIA_CHECK(lifecycle.bodyEnded());
    RUVIA_CHECK(lifecycle.closeSource() == Http2StreamCloseSource::kPeer);
}

RUVIA_TEST(stream_lifecycle_queue_then_dispatch) {
    Http2StreamLifecycle lifecycle;
    RUVIA_CHECK(lifecycle.tryMarkQueued());   // first queue succeeds
    RUVIA_CHECK(lifecycle.queued());
    RUVIA_CHECK(!lifecycle.tryMarkQueued());   // already queued
    RUVIA_CHECK(lifecycle.tryStartDispatch());  // starting dispatch clears the queued flag
    RUVIA_CHECK(!lifecycle.queued());
    RUVIA_CHECK(lifecycle.dispatchStarted());
    RUVIA_CHECK(!lifecycle.tryStartDispatch());  // dispatch starts at most once
}

RUVIA_TEST(stream_lifecycle_clear_queued_allows_requeue) {
    // clearQueued undoes tryMarkQueued WITHOUT starting dispatch -- the rollback used
    // when a ready-queue push fails after the stream was marked queued. It must
    // re-enable queuing, or a stream that failed to enqueue once could never be
    // scheduled again, leaving the request permanently hung.
    Http2StreamLifecycle lifecycle;
    RUVIA_CHECK(lifecycle.tryMarkQueued());
    RUVIA_CHECK(!lifecycle.tryMarkQueued());   // already queued
    lifecycle.clearQueued();
    RUVIA_CHECK(!lifecycle.queued());
    RUVIA_CHECK(lifecycle.tryMarkQueued());    // can be queued again after the rollback
    RUVIA_CHECK(lifecycle.queued());
    RUVIA_CHECK(!lifecycle.dispatchStarted()); // the rollback did not start dispatch
}

RUVIA_TEST(stream_lifecycle_reset_blocks_queue_and_dispatch) {
    // A reset stream must not be scheduled or dispatched (RFC 7540 5.1).
    Http2StreamLifecycle resetFirst;
    resetFirst.markReset(Http2StreamCloseSource::kLocal);
    RUVIA_CHECK(!resetFirst.tryMarkQueued());
    RUVIA_CHECK(!resetFirst.tryStartDispatch());

    // Queuing then resetting still blocks dispatch.
    Http2StreamLifecycle queuedThenReset;
    RUVIA_CHECK(queuedThenReset.tryMarkQueued());
    queuedThenReset.markReset(Http2StreamCloseSource::kPeer);
    RUVIA_CHECK(!queuedThenReset.tryStartDispatch());
}
