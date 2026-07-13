#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <string_view>

#include "ruvia/http/detail/http2/Http2FlowControl.h"

namespace {

using ruvia::detail::Http2ReceiveWindowDebitStatus;
using ruvia::detail::Http2StreamState;
using ruvia::detail::Http2WindowUpdateResult;
using ruvia::detail::http2ApplyStreamWindowUpdate;
using ruvia::detail::http2ApplyWindowUpdate;
using ruvia::detail::http2AvailableSendWindow;
using ruvia::detail::http2CreditConnectionReceiveWindow;
using ruvia::detail::http2CreditStreamReceiveWindow;
using ruvia::detail::http2ConsumeSendWindow;
using ruvia::detail::http2DebitConnectionReceiveWindow;
using ruvia::detail::http2DebitStreamReceiveWindow;
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

RUVIA_TEST(flow_available_send_window_clamps_to_zero_when_negative) {
    // RFC 7540 6.9.2: lowering SETTINGS_INITIAL_WINDOW_SIZE can drive a stream's
    // send window negative. The available-to-send count must clamp to zero and
    // never wrap to a huge size_t, which would let a full DATA frame be sent past
    // an exhausted window (a flow-control violation).
    auto stream = makeStream();
    const auto streamWindow = stream.sendWindow();
    RUVIA_CHECK(stream.addSendWindow(-(static_cast<std::int64_t>(streamWindow) + 1000)));
    RUVIA_CHECK(stream.sendWindow() < 0);

    RUVIA_CHECK_EQ(http2AvailableSendWindow(65535, stream), std::size_t{0});
    RUVIA_CHECK(!http2SendWindowAvailable(65535, stream));

    // A non-positive connection window is likewise clamped, not wrapped.
    RUVIA_CHECK_EQ(http2AvailableSendWindow(-5, makeStream()), std::size_t{0});
    RUVIA_CHECK_EQ(http2AvailableSendWindow(0, makeStream()), std::size_t{0});

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

RUVIA_TEST(flow_connection_receive_window_debit_is_transactional) {
    std::int32_t connection = 1000;
    RUVIA_CHECK(http2DebitConnectionReceiveWindow(connection, 2000) ==
        Http2ReceiveWindowDebitStatus::kExceeded);
    RUVIA_CHECK_EQ(connection, 1000);
    RUVIA_CHECK(http2DebitConnectionReceiveWindow(connection, 300) ==
        Http2ReceiveWindowDebitStatus::kAccepted);
    RUVIA_CHECK_EQ(connection, 700);
    http2CreditConnectionReceiveWindow(connection, 300);
    RUVIA_CHECK_EQ(connection, 1000);
}

RUVIA_TEST(flow_stream_receive_window_debit_is_separate_from_connection) {
    auto stream = makeStream();
    std::int32_t bigConnection = 2'000'000;
    constexpr std::int32_t bytes = 1'100'000;
    RUVIA_CHECK(http2DebitConnectionReceiveWindow(bigConnection, bytes) ==
        Http2ReceiveWindowDebitStatus::kAccepted);
    RUVIA_CHECK_EQ(bigConnection, 900'000);
    RUVIA_CHECK(http2DebitStreamReceiveWindow(stream, bytes) ==
        Http2ReceiveWindowDebitStatus::kExceeded);
    // A stream failure does not secretly roll back the already accepted connection
    // debit; the caller explicitly releases it when discarding the frame.
    RUVIA_CHECK_EQ(bigConnection, 900'000);
    http2CreditConnectionReceiveWindow(bigConnection, bytes);
    RUVIA_CHECK_EQ(bigConnection, 2'000'000);
}

RUVIA_TEST(flow_receive_window_credit_reopens_capacity) {
    auto stream = makeStream();
    std::int32_t connection = 500;
    RUVIA_CHECK(http2DebitConnectionReceiveWindow(connection, 200) ==
        Http2ReceiveWindowDebitStatus::kAccepted);
    RUVIA_CHECK(http2DebitStreamReceiveWindow(stream, 200) ==
        Http2ReceiveWindowDebitStatus::kAccepted);
    RUVIA_CHECK_EQ(connection, 300);
    http2CreditConnectionReceiveWindow(connection, 200);
    http2CreditStreamReceiveWindow(stream, 200);
    RUVIA_CHECK_EQ(connection, 500);
    // The stream window was restored too, so it can receive again.
    RUVIA_CHECK(http2DebitConnectionReceiveWindow(connection, 200) ==
        Http2ReceiveWindowDebitStatus::kAccepted);
    RUVIA_CHECK(http2DebitStreamReceiveWindow(stream, 200) ==
        Http2ReceiveWindowDebitStatus::kAccepted);
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
