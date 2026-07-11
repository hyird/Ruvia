#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/detail/http2/Http2BodyQueue.h"
#include "ruvia/http/detail/http2/Http2BodyState.h"

namespace {

using ruvia::detail::HttpRequestBodyMode;
using ruvia::detail::Http2BodyAccountingResult;
using ruvia::detail::Http2StreamState;
using ruvia::detail::http2AccountDataBody;
using ruvia::detail::httpRequestBodyByteLimit;

Http2StreamState makeStream() {
    return Http2StreamState(1, std::pmr::new_delete_resource());
}

}  // namespace

RUVIA_TEST(request_body_byte_limit_selects_by_mode) {
    RUVIA_CHECK_EQ(httpRequestBodyByteLimit(HttpRequestBodyMode::kStream, 100, 200), std::size_t{100});
    RUVIA_CHECK_EQ(httpRequestBodyByteLimit(HttpRequestBodyMode::kBuffered, 100, 200), std::size_t{200});
}

RUVIA_TEST(h2_account_data_body_ok_and_accumulates) {
    auto stream = makeStream();
    RUVIA_CHECK(http2AccountDataBody(stream, 40, 0, 1000) == Http2BodyAccountingResult::kOk);
    RUVIA_CHECK(http2AccountDataBody(stream, 60, 0, 1000) == Http2BodyAccountingResult::kOk);
    RUVIA_CHECK_EQ(
        stream.remoteContent().allowedWithoutLength()->receivedBytes(),
        std::size_t{100});
}

RUVIA_TEST(h2_account_data_body_too_large) {
    // A single frame beyond the buffered limit is rejected.
    auto stream = makeStream();
    RUVIA_CHECK(http2AccountDataBody(stream, 101, 0, 100) == Http2BodyAccountingResult::kTooLarge);

    // Accumulated overflow: 60 fits, the next 50 pushes past 100 (checked before
    // the bytes are added, so it is overflow-safe).
    auto accumulate = makeStream();
    RUVIA_CHECK(http2AccountDataBody(accumulate, 60, 0, 100) == Http2BodyAccountingResult::kOk);
    RUVIA_CHECK(http2AccountDataBody(accumulate, 50, 0, 100) == Http2BodyAccountingResult::kTooLarge);
    RUVIA_CHECK_EQ(
        accumulate.remoteContent().allowedWithoutLength()->receivedBytes(),
        std::size_t{60});  // rejected bytes not counted
}

RUVIA_TEST(h2_account_data_body_bounds_streaming_backlog) {
    // A streaming route has no total-size cap (maxStreamBodyBytes = 0, so arbitrarily
    // large uploads are allowed), but the *un-drained* backlog must stay bounded by
    // maxBufferedBodyBytes: the receive window is re-credited on every DATA frame, so a
    // peer that outruns the handler would otherwise grow memory without limit.
    auto stream = makeStream();
    stream.setBodyMode(HttpRequestBodyMode::kStream);
    stream.enqueueBodyChunk(std::string(90, 'x'));  // backlog the handler has not drained
    RUVIA_CHECK_EQ(stream.queuedBodyBytes(), std::size_t{90});

    // 90 backlog + 10 == 100 fits exactly; the total (maxStreamBodyBytes = 0) is unbounded.
    RUVIA_CHECK(http2AccountDataBody(stream, 10, 0, 100) == Http2BodyAccountingResult::kOk);
    // 90 backlog + 11 exceeds the 100-byte backlog cap -> reset (checked before the chunk
    // is enqueued, so it is overflow-safe).
    RUVIA_CHECK(http2AccountDataBody(stream, 11, 0, 100) == Http2BodyAccountingResult::kTooLarge);

    // Draining lets the peer send again: an unbounded total upload succeeds as long as
    // the handler keeps the backlog under the cap.
    RUVIA_CHECK_EQ(stream.popBodyChunk().size(), std::size_t{90});
    RUVIA_CHECK_EQ(stream.queuedBodyBytes(), std::size_t{0});
    RUVIA_CHECK(http2AccountDataBody(stream, 100, 0, 100) == Http2BodyAccountingResult::kOk);

    // maxBufferedBodyBytes = 0 disables the backlog cap -- an explicit opt-in to
    // unbounded buffering, matching the "0 = unbounded" convention of the other limits.
    auto unbounded = makeStream();
    unbounded.setBodyMode(HttpRequestBodyMode::kStream);
    unbounded.enqueueBodyChunk(std::string(200, 'z'));
    RUVIA_CHECK(http2AccountDataBody(unbounded, 100000, 0, 0) == Http2BodyAccountingResult::kOk);

    // Buffered mode ignores the backlog cap (its memory is bounded by the total cap
    // instead): the same backlog + a frame that trips the streaming cap is accepted.
    auto buffered = makeStream();
    buffered.setBodyMode(HttpRequestBodyMode::kBuffered);
    buffered.enqueueBodyChunk(std::string(90, 'y'));
    RUVIA_CHECK(http2AccountDataBody(buffered, 50, 0, 100) == Http2BodyAccountingResult::kOk);
}

RUVIA_TEST(h2_account_data_body_content_length_exceeded) {
    auto stream = makeStream();
    RUVIA_CHECK(stream.declareRemoteContentLength(50));
    // Byte limits are unbounded, so only the Content-Length check trips
    // (RFC 9113 8.1.1).
    RUVIA_CHECK(http2AccountDataBody(stream, 60, 0, 0) ==
                Http2BodyAccountingResult::kContentLengthExceeded);
    RUVIA_CHECK_EQ(
        stream.remoteContent().allowedKnownLength()->receivedBytes(),
        std::size_t{0});
}

RUVIA_TEST(h2_account_data_body_rejects_metadata_only_payload_before_product_limits) {
    auto stream = makeStream();
    RUVIA_CHECK(stream.declareRemoteContentLength(42));
    RUVIA_CHECK(stream.selectRemoteContentMetadataOnly());
    RUVIA_CHECK(http2AccountDataBody(stream, 1, 0, 0) ==
        Http2BodyAccountingResult::kContentForbidden);
    RUVIA_CHECK(stream.remoteContent().metadataOnlyKnownLength() != nullptr);
    RUVIA_CHECK(http2AccountDataBody(stream, 0, 0, 0) ==
        Http2BodyAccountingResult::kOk);
}

RUVIA_TEST(h2_remote_content_terminal_validation_is_owned_by_active_alternative) {
    auto ordinary = makeStream();
    RUVIA_CHECK(ordinary.declareRemoteContentLength(42));
    RUVIA_CHECK(!ordinary.remoteContent().terminalLengthValid());

    auto metadataOnly = makeStream();
    RUVIA_CHECK(metadataOnly.declareRemoteContentLength(42));
    RUVIA_CHECK(metadataOnly.selectRemoteContentMetadataOnly());
    RUVIA_CHECK(metadataOnly.remoteContent().terminalLengthValid());
}

RUVIA_TEST(h2_account_data_body_mode_selects_limit) {
    // Stream mode applies the stream limit.
    auto streamMode = makeStream();
    streamMode.setBodyMode(HttpRequestBodyMode::kStream);
    RUVIA_CHECK(http2AccountDataBody(streamMode, 150, 100, 0) == Http2BodyAccountingResult::kTooLarge);

    // Buffered mode applies the buffered limit.
    auto bufferedMode = makeStream();
    bufferedMode.setBodyMode(HttpRequestBodyMode::kBuffered);
    RUVIA_CHECK(http2AccountDataBody(bufferedMode, 150, 0, 100) == Http2BodyAccountingResult::kTooLarge);

    // A 0 limit means unbounded, so a large frame is accepted.
    auto unbounded = makeStream();
    RUVIA_CHECK(http2AccountDataBody(unbounded, 100000, 0, 0) == Http2BodyAccountingResult::kOk);
}
