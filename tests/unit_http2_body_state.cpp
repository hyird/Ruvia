#include "test_harness.h"

#include <cstddef>
#include <memory_resource>

#include "net/http2/Http2BodyState.h"

namespace {

using ruvia::RequestBodyMode;
using ruvia::detail::Http2BodyAccountingResult;
using ruvia::detail::Http2StreamState;
using ruvia::detail::http2AccountDataBody;
using ruvia::detail::http2BodyLengthComplete;
using ruvia::detail::requestBodyByteLimit;

Http2StreamState makeStream() {
    return Http2StreamState(1, std::pmr::new_delete_resource());
}

}  // namespace

RUVIA_TEST(request_body_byte_limit_selects_by_mode) {
    RUVIA_CHECK_EQ(requestBodyByteLimit(RequestBodyMode::kStream, 100, 200), std::size_t{100});
    RUVIA_CHECK_EQ(requestBodyByteLimit(RequestBodyMode::kBuffered, 100, 200), std::size_t{200});
}

RUVIA_TEST(h2_account_data_body_ok_and_accumulates) {
    auto stream = makeStream();
    RUVIA_CHECK(http2AccountDataBody(stream, 40, 0, 1000) == Http2BodyAccountingResult::kOk);
    RUVIA_CHECK(http2AccountDataBody(stream, 60, 0, 1000) == Http2BodyAccountingResult::kOk);
    RUVIA_CHECK_EQ(stream.receivedBodyBytes(), std::size_t{100});
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
    RUVIA_CHECK_EQ(accumulate.receivedBodyBytes(), std::size_t{60});  // rejected bytes not counted
}

RUVIA_TEST(h2_account_data_body_content_length_exceeded) {
    auto stream = makeStream();
    RUVIA_CHECK(stream.setContentLength(50));
    // Byte limits are unbounded, so only the Content-Length check trips
    // (RFC 7540 8.1.2.6).
    RUVIA_CHECK(http2AccountDataBody(stream, 60, 0, 0) ==
                Http2BodyAccountingResult::kContentLengthExceeded);
}

RUVIA_TEST(h2_account_data_body_websocket_tunnel_bypasses_limits) {
    auto stream = makeStream();
    stream.markWebSocketTunnel();
    // A tiny limit, but a tunnel bypasses request-body accounting entirely.
    RUVIA_CHECK(http2AccountDataBody(stream, 100000, 1, 1) == Http2BodyAccountingResult::kOk);
    RUVIA_CHECK(http2BodyLengthComplete(stream));  // a tunnel is always length-complete
}

RUVIA_TEST(h2_account_data_body_mode_selects_limit) {
    // Stream mode applies the stream limit.
    auto streamMode = makeStream();
    streamMode.setBodyMode(RequestBodyMode::kStream);
    RUVIA_CHECK(http2AccountDataBody(streamMode, 150, 100, 0) == Http2BodyAccountingResult::kTooLarge);

    // Buffered mode applies the buffered limit.
    auto bufferedMode = makeStream();
    bufferedMode.setBodyMode(RequestBodyMode::kBuffered);
    RUVIA_CHECK(http2AccountDataBody(bufferedMode, 150, 0, 100) == Http2BodyAccountingResult::kTooLarge);

    // A 0 limit means unbounded, so a large frame is accepted.
    auto unbounded = makeStream();
    RUVIA_CHECK(http2AccountDataBody(unbounded, 100000, 0, 0) == Http2BodyAccountingResult::kOk);
}
