#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <string_view>

#include "http2/http2_flow_control.h"
#include "test_harness.h"

namespace {

using ruvia::detail::http2_apply_stream_window_update;
using ruvia::detail::http2_apply_window_update;
using ruvia::detail::http2_available_send_window;
using ruvia::detail::http2_consume_send_window;
using ruvia::detail::http2_credit_connection_receive_window;
using ruvia::detail::http2_credit_stream_receive_window;
using ruvia::detail::http2_debit_connection_receive_window;
using ruvia::detail::http2_debit_stream_receive_window;
using ruvia::detail::http2_receive_window_debit_status;
using ruvia::detail::http2_send_window_available;
using ruvia::detail::http2_stream_state;
using ruvia::detail::http2_window_update_increment;
using ruvia::detail::http2_window_update_result;

constexpr std::int32_t int32_max = (std::numeric_limits<std::int32_t>::max)();

http2_stream_state make_stream() {
    return http2_stream_state(1, std::pmr::new_delete_resource());
}

}  // namespace

RUVIA_TEST(flow_window_update_overflow_guard) {
    std::int32_t window = 100;
    // A zero increment is a protocol error and leaves the window untouched.
    RUVIA_CHECK(http2_apply_window_update(window, 0) == http2_window_update_result::zero_increment);
    RUVIA_CHECK_EQ(window, 100);
    // A normal increment advances the window.
    RUVIA_CHECK(http2_apply_window_update(window, 50) == http2_window_update_result::ok);
    RUVIA_CHECK_EQ(window, 150);

    // Exceeding 2^31-1 is rejected (RFC 7540 6.9.1) and leaves the window intact.
    std::int32_t high = int32_max - 10;
    RUVIA_CHECK(http2_apply_window_update(high, 20) == http2_window_update_result::overflow);
    RUVIA_CHECK_EQ(high, int32_max - 10);
    // Reaching exactly 2^31-1 is allowed.
    RUVIA_CHECK(http2_apply_window_update(high, 10) == http2_window_update_result::ok);
    RUVIA_CHECK_EQ(high, int32_max);
}

RUVIA_TEST(flow_window_update_increment_reads_31_bits) {
    // The reserved high bit of the increment must be masked off.
    const char payload_value[] = {static_cast<char>(0x80), 0, 0, 5};
    RUVIA_CHECK_EQ(http2_window_update_increment(std::string_view(payload_value, 4)), std::uint32_t{5});
}

RUVIA_TEST(flow_send_window_available_is_min_of_connection_and_stream) {
    auto stream = make_stream();
    const auto stream_window = stream.send_window();
    RUVIA_CHECK(stream_window > 0);

    // Available to send is the minimum of the connection and stream windows.
    RUVIA_CHECK_EQ(http2_available_send_window(stream_window + 100, stream),
        static_cast<std::size_t>(stream_window));
    RUVIA_CHECK_EQ(http2_available_send_window(stream_window - 1, stream),
        static_cast<std::size_t>(stream_window - 1));

    // Sending is possible only when both windows are positive.
    RUVIA_CHECK(http2_send_window_available(100, stream));
    RUVIA_CHECK(!http2_send_window_available(0, stream));
}

RUVIA_TEST(flow_available_send_window_clamps_to_zero_when_negative) {
    // RFC 7540 6.9.2: lowering SETTINGS_INITIAL_WINDOW_SIZE can drive a stream's
    // send window negative. The available-to-send count must clamp to zero and
    // never wrap to a huge size_t, which would let a full DATA frame be sent past
    // an exhausted window (a flow-control violation).
    auto stream = make_stream();
    const auto stream_window = stream.send_window();
    RUVIA_CHECK(stream.add_send_window(-(static_cast<std::int64_t>(stream_window) + 1000)));
    RUVIA_CHECK(stream.send_window() < 0);

    RUVIA_CHECK_EQ(http2_available_send_window(65535, stream), std::size_t{0});
    RUVIA_CHECK(!http2_send_window_available(65535, stream));

    // A non-positive connection window is likewise clamped, not wrapped.
    RUVIA_CHECK_EQ(http2_available_send_window(-5, make_stream()), std::size_t{0});
    RUVIA_CHECK_EQ(http2_available_send_window(0, make_stream()), std::size_t{0});

    stream.set_send_window(100);
    RUVIA_CHECK_EQ(http2_available_send_window(0, stream), std::size_t{0});
    RUVIA_CHECK_EQ(http2_available_send_window(-10, stream), std::size_t{0});
}

RUVIA_TEST(flow_consume_send_window_deducts_both) {
    auto stream = make_stream();
    const auto before_stream = stream.send_window();
    std::int32_t connection_window = 1000;
    http2_consume_send_window(connection_window, stream, 200);
    RUVIA_CHECK_EQ(connection_window, 800);
    RUVIA_CHECK_EQ(stream.send_window(), before_stream - 200);
}

RUVIA_TEST(flow_connection_receive_window_debit_is_transactional) {
    std::int32_t connection = 1000;
    RUVIA_CHECK(http2_debit_connection_receive_window(connection, 2000) ==
                http2_receive_window_debit_status::exceeded);
    RUVIA_CHECK_EQ(connection, 1000);
    RUVIA_CHECK(http2_debit_connection_receive_window(connection, 300) ==
                http2_receive_window_debit_status::accepted);
    RUVIA_CHECK_EQ(connection, 700);
    http2_credit_connection_receive_window(connection, 300);
    RUVIA_CHECK_EQ(connection, 1000);
}

RUVIA_TEST(flow_stream_receive_window_debit_is_separate_from_connection) {
    auto stream = make_stream();
    std::int32_t big_connection = 2'000'000;
    constexpr std::int32_t bytes_value = 1'100'000;
    RUVIA_CHECK(http2_debit_connection_receive_window(big_connection, bytes_value) ==
                http2_receive_window_debit_status::accepted);
    RUVIA_CHECK_EQ(big_connection, 900'000);
    RUVIA_CHECK(
        http2_debit_stream_receive_window(stream, bytes_value) == http2_receive_window_debit_status::exceeded);
    // A stream failure does not secretly roll back the already accepted connection
    // debit; the caller explicitly releases it when discarding the frame.
    RUVIA_CHECK_EQ(big_connection, 900'000);
    http2_credit_connection_receive_window(big_connection, bytes_value);
    RUVIA_CHECK_EQ(big_connection, 2'000'000);
}

RUVIA_TEST(flow_receive_window_credit_reopens_capacity) {
    auto stream = make_stream();
    std::int32_t connection = 500;
    RUVIA_CHECK(http2_debit_connection_receive_window(connection, 200) ==
                http2_receive_window_debit_status::accepted);
    RUVIA_CHECK(
        http2_debit_stream_receive_window(stream, 200) == http2_receive_window_debit_status::accepted);
    RUVIA_CHECK_EQ(connection, 300);
    http2_credit_connection_receive_window(connection, 200);
    http2_credit_stream_receive_window(stream, 200);
    RUVIA_CHECK_EQ(connection, 500);
    // The stream window was restored too, so it can receive again.
    RUVIA_CHECK(http2_debit_connection_receive_window(connection, 200) ==
                http2_receive_window_debit_status::accepted);
    RUVIA_CHECK(
        http2_debit_stream_receive_window(stream, 200) == http2_receive_window_debit_status::accepted);
}

RUVIA_TEST(flow_apply_stream_window_update) {
    auto stream = make_stream();
    RUVIA_CHECK(http2_apply_stream_window_update(stream, 0) == http2_window_update_result::zero_increment);
    const auto before = stream.send_window();
    RUVIA_CHECK(http2_apply_stream_window_update(stream, 100) == http2_window_update_result::ok);
    RUVIA_CHECK_EQ(stream.send_window(), before + 100);

    // An increment that would push the send window past 2^31-1 overflows.
    auto overflowing = make_stream();
    RUVIA_CHECK(http2_apply_stream_window_update(overflowing, static_cast<std::uint32_t>(int32_max)) ==
                http2_window_update_result::overflow);
}
