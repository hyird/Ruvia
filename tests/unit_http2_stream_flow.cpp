#include "test_harness.h"

#include <cstdint>
#include <limits>

#include "net/http2/Http2StreamFlowControl.h"

namespace {

using ruvia::detail::Http2StreamFlowControl;

}  // namespace

RUVIA_TEST(http2_stream_send_window_bounds) {
    Http2StreamFlowControl fc;
    RUVIA_CHECK_EQ(fc.sendWindow(), std::int32_t{65535});  // default initial window
    RUVIA_CHECK(fc.addSendWindow(100));
    RUVIA_CHECK_EQ(fc.sendWindow(), std::int32_t{65635});

    constexpr auto kMax = std::numeric_limits<std::int32_t>::max();
    fc.setSendWindow(0);
    RUVIA_CHECK(fc.addSendWindow(kMax));  // reaching exactly 2^31-1 is allowed
    RUVIA_CHECK_EQ(fc.sendWindow(), kMax);
    RUVIA_CHECK(!fc.addSendWindow(1));    // one more overflows -> refused (RFC 7540 6.9.1)
    RUVIA_CHECK_EQ(fc.sendWindow(), kMax);  // and leaves the window unchanged
}

RUVIA_TEST(http2_stream_send_window_can_go_negative) {
    // A SETTINGS_INITIAL_WINDOW_SIZE reduction can push a send window negative
    // (RFC 7540 6.9.2); that is not an overflow.
    Http2StreamFlowControl fc;
    fc.setSendWindow(100);
    RUVIA_CHECK(fc.addSendWindow(-500));
    RUVIA_CHECK_EQ(fc.sendWindow(), std::int32_t{-400});
    // A delta below INT32_MIN is rejected.
    fc.setSendWindow(std::numeric_limits<std::int32_t>::min());
    RUVIA_CHECK(!fc.addSendWindow(-1));
    // consumeSend reduces the window by the bytes sent.
    fc.setSendWindow(1000);
    fc.consumeSend(300);
    RUVIA_CHECK_EQ(fc.sendWindow(), std::int32_t{700});
}

RUVIA_TEST(http2_stream_receive_window_enforced) {
    Http2StreamFlowControl fc;  // receive window starts at 1 MiB
    // Consuming more than the window is refused (flow control).
    RUVIA_CHECK(!fc.consumeReceive(std::numeric_limits<std::int32_t>::max()));
    // A modest amount within the window succeeds; restoreReceive returns the space.
    RUVIA_CHECK(fc.consumeReceive(1000));
    fc.restoreReceive(1000);
    RUVIA_CHECK(fc.consumeReceive(1000));
}
