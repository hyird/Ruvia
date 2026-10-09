#include <cstdint>
#include <limits>

#include "http2/http2_stream_flow_control.h"
#include "test_harness.h"

namespace {

using ruvia::detail::http2_stream_flow_control;

}  // namespace

RUVIA_TEST(http2_stream_send_window_bounds) {
    http2_stream_flow_control fc;
    RUVIA_CHECK_EQ(fc.send_window(), std::int32_t{65535});  // default initial window
    RUVIA_CHECK(fc.add_send_window(100));
    RUVIA_CHECK_EQ(fc.send_window(), std::int32_t{65635});

    constexpr auto max_value = std::numeric_limits<std::int32_t>::max();
    fc.set_send_window(0);
    RUVIA_CHECK(fc.add_send_window(max_value));  // reaching exactly 2^31-1 is allowed
    RUVIA_CHECK_EQ(fc.send_window(), max_value);
    RUVIA_CHECK(!fc.add_send_window(1));          // one more overflows -> refused (RFC 7540 6.9.1)
    RUVIA_CHECK_EQ(fc.send_window(), max_value);  // and leaves the window unchanged
}

RUVIA_TEST(http2_stream_send_window_can_go_negative) {
    // A SETTINGS_INITIAL_WINDOW_SIZE reduction can push a send window negative
    // (RFC 7540 6.9.2); that is not an overflow.
    http2_stream_flow_control fc;
    fc.set_send_window(100);
    RUVIA_CHECK(fc.add_send_window(-500));
    RUVIA_CHECK_EQ(fc.send_window(), std::int32_t{-400});
    // A delta below INT32_MIN is rejected.
    fc.set_send_window(std::numeric_limits<std::int32_t>::min());
    RUVIA_CHECK(!fc.add_send_window(-1));
    // consume_send reduces the window by the bytes sent.
    fc.set_send_window(1000);
    fc.consume_send(300);
    RUVIA_CHECK_EQ(fc.send_window(), std::int32_t{700});
}

RUVIA_TEST(http2_stream_receive_window_enforced) {
    http2_stream_flow_control fc;  // receive window starts at 1 MiB
    // Consuming more than the window is refused (flow control).
    RUVIA_CHECK(!fc.consume_receive(std::numeric_limits<std::int32_t>::max()));
    // A modest amount within the window succeeds; restore_receive returns the space.
    RUVIA_CHECK(fc.consume_receive(1000));
    fc.restore_receive(1000);
    RUVIA_CHECK(fc.consume_receive(1000));
}
