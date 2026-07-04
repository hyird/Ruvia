#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <string_view>

#include "net/http2/Http2FlowControl.h"

namespace {

using ruvia::detail::Http2ReceiveWindowResult;
using ruvia::detail::Http2StreamState;
using ruvia::detail::Http2WindowUpdateResult;
using ruvia::detail::http2ApplyStreamWindowUpdate;
using ruvia::detail::http2ApplyWindowUpdate;
using ruvia::detail::http2AvailableSendWindow;
using ruvia::detail::http2ConsumeReceiveWindows;
using ruvia::detail::http2ConsumeSendWindow;
using ruvia::detail::http2RestoreReceiveWindows;
using ruvia::detail::http2SendWindowAvailable;
using ruvia::detail::http2WindowUpdateIncrement;

constexpr std::int32_t kInt32Max = (std::numeric_limits<std::int32_t>::max)();

Http2StreamState makeStream() {
    return Http2StreamState(1, std::pmr::new_delete_resource());
}

}  // namespace

RUVIA_TEST(flow_window_update_overflow_guard) {
    std::int32_t window = 100;
    // A zero increment is a protocol error and leaves the window untouched.
    RUVIA_CHECK(http2ApplyWindowUpdate(window, 0) == Http2WindowUpdateResult::kZeroIncrement);
    RUVIA_CHECK_EQ(window, 100);
    // A normal increment advances the window.
    RUVIA_CHECK(http2ApplyWindowUpdate(window, 50) == Http2WindowUpdateResult::kOk);
    RUVIA_CHECK_EQ(window, 150);

    // Exceeding 2^31-1 is rejected (RFC 7540 6.9.1) and leaves the window intact.
    std::int32_t high = kInt32Max - 10;
    RUVIA_CHECK(http2ApplyWindowUpdate(high, 20) == Http2WindowUpdateResult::kOverflow);
    RUVIA_CHECK_EQ(high, kInt32Max - 10);
    // Reaching exactly 2^31-1 is allowed.
    RUVIA_CHECK(http2ApplyWindowUpdate(high, 10) == Http2WindowUpdateResult::kOk);
    RUVIA_CHECK_EQ(high, kInt32Max);
}

RUVIA_TEST(flow_window_update_increment_reads_31_bits) {
    // The reserved high bit of the increment must be masked off.
    const char payload[] = {static_cast<char>(0x80), 0, 0, 5};
    RUVIA_CHECK_EQ(http2WindowUpdateIncrement(std::string_view(payload, 4)), std::uint32_t{5});
}

RUVIA_TEST(flow_send_window_available_is_min_of_connection_and_stream) {
    auto stream = makeStream();
    const auto streamWindow = stream.sendWindow();
    RUVIA_CHECK(streamWindow > 0);

    // Available to send is the minimum of the connection and stream windows.
    RUVIA_CHECK_EQ(http2AvailableSendWindow(streamWindow + 100, stream),
                   static_cast<std::size_t>(streamWindow));
    RUVIA_CHECK_EQ(http2AvailableSendWindow(streamWindow - 1, stream),
                   static_cast<std::size_t>(streamWindow - 1));

    // Sending is possible only when both windows are positive.
    RUVIA_CHECK(http2SendWindowAvailable(100, stream));
    RUVIA_CHECK(!http2SendWindowAvailable(0, stream));
}

RUVIA_TEST(flow_available_send_window_clamps_non_positive) {
    auto stream = makeStream();
    stream.setSendWindow(-10);
    RUVIA_CHECK_EQ(http2AvailableSendWindow(100, stream), std::size_t{0});

    stream.setSendWindow(100);
    RUVIA_CHECK_EQ(http2AvailableSendWindow(0, stream), std::size_t{0});
    RUVIA_CHECK_EQ(http2AvailableSendWindow(-10, stream), std::size_t{0});
}

RUVIA_TEST(flow_consume_send_window_deducts_both) {
    auto stream = makeStream();
    const auto beforeStream = stream.sendWindow();
    std::int32_t connectionWindow = 1000;
    http2ConsumeSendWindow(connectionWindow, stream, 200);
    RUVIA_CHECK_EQ(connectionWindow, 800);
    RUVIA_CHECK_EQ(stream.sendWindow(), beforeStream - 200);
}

RUVIA_TEST(flow_consume_receive_window_enforces_connection_then_stream) {
    auto stream = makeStream();
    std::int32_t connection = 1000;
    // More than the connection window is rejected without mutating it.
    RUVIA_CHECK(http2ConsumeReceiveWindows(connection, stream, 2000) ==
                Http2ReceiveWindowResult::kConnectionExceeded);
    RUVIA_CHECK_EQ(connection, 1000);
    // A normal receive deducts the connection window.
    RUVIA_CHECK(http2ConsumeReceiveWindows(connection, stream, 300) ==
                Http2ReceiveWindowResult::kOk);
    RUVIA_CHECK_EQ(connection, 700);

    // Within the connection window but beyond the stream's 1 MiB receive window
    // is a stream error.
    auto other = makeStream();
    std::int32_t bigConnection = 2'000'000;
    RUVIA_CHECK(http2ConsumeReceiveWindows(bigConnection, other, 1'100'000) ==
                Http2ReceiveWindowResult::kStreamExceeded);
    RUVIA_CHECK_EQ(bigConnection, 2'000'000);  // unchanged when the stream is exceeded
}

RUVIA_TEST(flow_restore_receive_window_reopens_capacity) {
    auto stream = makeStream();
    std::int32_t connection = 500;
    RUVIA_CHECK(http2ConsumeReceiveWindows(connection, stream, 200) ==
                Http2ReceiveWindowResult::kOk);
    RUVIA_CHECK_EQ(connection, 300);
    http2RestoreReceiveWindows(connection, stream, 200);
    RUVIA_CHECK_EQ(connection, 500);
    // The stream window was restored too, so it can receive again.
    RUVIA_CHECK(http2ConsumeReceiveWindows(connection, stream, 200) ==
                Http2ReceiveWindowResult::kOk);
}

RUVIA_TEST(flow_apply_stream_window_update) {
    auto stream = makeStream();
    RUVIA_CHECK(http2ApplyStreamWindowUpdate(stream, 0) == Http2WindowUpdateResult::kZeroIncrement);
    const auto before = stream.sendWindow();
    RUVIA_CHECK(http2ApplyStreamWindowUpdate(stream, 100) == Http2WindowUpdateResult::kOk);
    RUVIA_CHECK_EQ(stream.sendWindow(), before + 100);

    // An increment that would push the send window past 2^31-1 overflows.
    auto overflowing = makeStream();
    RUVIA_CHECK(http2ApplyStreamWindowUpdate(overflowing, static_cast<std::uint32_t>(kInt32Max)) ==
                Http2WindowUpdateResult::kOverflow);
}
