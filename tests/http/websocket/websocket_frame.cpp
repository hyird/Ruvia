#include "test_harness.h"

#include <array>
#include <cstddef>
#include <concepts>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/detail/websocket/HttpWebSocketFrameCodec.h"
#include "ruvia/http/detail/websocket/HttpWebSocketFrameReader.h"
#include "ruvia/http/detail/websocket/HttpWebSocketFrameView.h"
#include "ruvia/http/WebSocketProtocol.h"

namespace {

using ruvia::ProtocolByteLimit;
using ruvia::WebSocketOpcode;
using ruvia::detail::decodeMaskedWebSocketPayload;
using ruvia::detail::decodeWebSocketFrameStart;
using ruvia::detail::encodeWebSocketFrameHeader;
using ruvia::detail::isInvalidWebSocketControlFrame;
using ruvia::detail::WebSocketFrameHeader;
using ruvia::detail::WebSocketFrameKind;
using ruvia::detail::WebSocketFrameReadResult;
using ruvia::detail::WebSocketFrameStart;
using ruvia::detail::WebSocketFrameView;
using ruvia::detail::WebSocketProtocolFailure;
using ruvia::detail::webSocketFrameLengthExceedsLimit;
using ruvia::detail::webSocketTryReadFrame;

template <typename T>
concept HasFrameReadStatusField = requires(const T& result) {
    result.status;
};

template <typename T>
concept HasFrameReadStatusAccessor = requires(const T& result) {
    result.status();
};

template <typename T>
concept HasRequiredBytesField = requires(const T& result) {
    result.requiredBytes;
};

template <typename T>
concept HasCleanEofAllowedField = requires(const T& result) {
    result.cleanEofAllowed;
};

template <typename T>
concept HasFrameReadError = requires(const T& result) {
    { result.error() } -> std::same_as<WebSocketProtocolFailure>;
};

template <typename T>
concept HasAnyRvalueFrameReadAccessor =
    requires(T&& result) { std::move(result).needInput(); } ||
    requires(T&& result) { std::move(result).frame(); } ||
    requires(T&& result) { std::move(result).failure(); };

template <typename String>
concept AcceptsTemporaryTextFramePayload = requires(String&& payload) {
    WebSocketFrameView::text(std::move(payload), true);
};

template <typename String>
concept AcceptsTemporaryBinaryFramePayload = requires(String&& payload) {
    WebSocketFrameView::binary(std::move(payload), true);
};

template <typename String>
concept AcceptsTemporaryContinuationFramePayload = requires(String&& payload) {
    WebSocketFrameView::continuation(std::move(payload), true);
};

template <typename String>
concept AcceptsTemporaryCloseFramePayload = requires(String&& payload) {
    WebSocketFrameView::close(std::move(payload));
};

template <typename String>
concept AcceptsTemporaryPingFramePayload = requires(String&& payload) {
    WebSocketFrameView::ping(std::move(payload));
};

template <typename String>
concept AcceptsTemporaryPongFramePayload = requires(String&& payload) {
    WebSocketFrameView::pong(std::move(payload));
};

static_assert(!std::default_initializable<WebSocketFrameReadResult>);
static_assert(!std::default_initializable<WebSocketFrameStart>);
static_assert(!std::default_initializable<WebSocketFrameView>);
static_assert(std::same_as<
    decltype(std::declval<const WebSocketFrameReadResult&>().needInput()),
    const ruvia::detail::WebSocketFrameNeedInput*>);
static_assert(std::same_as<
    decltype(std::declval<const WebSocketFrameReadResult&>().frame()),
    const ruvia::detail::WebSocketFrameView*>);
static_assert(std::same_as<
    decltype(std::declval<const WebSocketFrameReadResult&>().failure()),
    const ruvia::detail::WebSocketFrameReadFailure*>);
static_assert(!HasFrameReadStatusField<WebSocketFrameReadResult>);
static_assert(!HasFrameReadStatusAccessor<WebSocketFrameReadResult>);
static_assert(!HasRequiredBytesField<WebSocketFrameReadResult>);
static_assert(!HasCleanEofAllowedField<WebSocketFrameReadResult>);
static_assert(!HasFrameReadError<WebSocketFrameReadResult>);
static_assert(!HasAnyRvalueFrameReadAccessor<WebSocketFrameReadResult>);
static_assert(HasFrameReadError<ruvia::detail::WebSocketFrameReadFailure>);
static_assert(!AcceptsTemporaryTextFramePayload<std::string>);
static_assert(!AcceptsTemporaryBinaryFramePayload<std::string>);
static_assert(!AcceptsTemporaryContinuationFramePayload<std::string>);
static_assert(!AcceptsTemporaryCloseFramePayload<std::string>);
static_assert(!AcceptsTemporaryPingFramePayload<std::string>);
static_assert(!AcceptsTemporaryPongFramePayload<std::string>);

std::pmr::string maskedFrame(
    unsigned char first,
    std::string_view payload) {
    constexpr std::array<unsigned char, 4> mask{0x12, 0x34, 0x56, 0x78};
    std::pmr::string bytes(std::pmr::get_default_resource());
    bytes.reserve(2 + mask.size() + payload.size());
    bytes.push_back(static_cast<char>(first));
    bytes.push_back(static_cast<char>(0x80U | payload.size()));
    for (const auto byte : mask) {
        bytes.push_back(static_cast<char>(byte));
    }
    for (std::size_t index = 0; index < payload.size(); ++index) {
        const auto byte = static_cast<unsigned char>(payload[index]);
        bytes.push_back(static_cast<char>(byte ^ mask[index & 3U]));
    }
    return bytes;
}

}  // namespace

// RFC 6455 §5.2: server-received client frames must be masked (second byte high
// bit set), RSV2/RSV3 must be clear, and reserved opcodes are rejected.
RUVIA_TEST(ws_frame_start_accepts_valid_masked_frames) {
    const auto text = decodeWebSocketFrameStart(0x81, 0x80, false);
    RUVIA_CHECK(text.has_value());
    RUVIA_CHECK(text->final());
    RUVIA_CHECK(!text->compressed());
    RUVIA_CHECK(text->kind() == WebSocketFrameKind::kText);
    const auto binary = decodeWebSocketFrameStart(0x82, 0x80, false);
    RUVIA_CHECK(binary->kind() == WebSocketFrameKind::kBinary);
    const auto close = decodeWebSocketFrameStart(0x88, 0x80, false);
    RUVIA_CHECK(close->kind() == WebSocketFrameKind::kClose);
    RUVIA_CHECK(decodeWebSocketFrameStart(0x89, 0x80, false)->kind() ==
        WebSocketFrameKind::kPing);
    RUVIA_CHECK(decodeWebSocketFrameStart(0x8A, 0x80, false)->kind() ==
        WebSocketFrameKind::kPong);
    const auto continuation = decodeWebSocketFrameStart(0x80, 0x80, false);
    RUVIA_CHECK(continuation->kind() == WebSocketFrameKind::kContinuation);
    const auto fragmentStart = decodeWebSocketFrameStart(0x01, 0x80, false);
    RUVIA_CHECK(!fragmentStart->final());
}

RUVIA_TEST(ws_frame_start_rejects_malformed) {
    RUVIA_CHECK(!decodeWebSocketFrameStart(0x81, 0x00, false));  // not masked
    RUVIA_CHECK(!decodeWebSocketFrameStart(0x91, 0x80, false));  // RSV2 set
    RUVIA_CHECK(!decodeWebSocketFrameStart(0xA1, 0x80, false));  // RSV3 set
    RUVIA_CHECK(!decodeWebSocketFrameStart(0x83, 0x80, false));  // reserved opcode 0x3
    RUVIA_CHECK(!decodeWebSocketFrameStart(0x87, 0x80, false));  // reserved opcode 0x7
    RUVIA_CHECK(!decodeWebSocketFrameStart(0x8B, 0x80, false));  // reserved control 0xB
    RUVIA_CHECK(!decodeWebSocketFrameStart(0x8F, 0x80, false));  // reserved control 0xF
}

RUVIA_TEST(ws_frame_start_rsv1_rules) {
    // RSV1 (compression) is valid only on the first data frame when negotiated.
    RUVIA_CHECK(!decodeWebSocketFrameStart(0xC1, 0x80, false));
    const auto compressed = decodeWebSocketFrameStart(0xC1, 0x80, true);
    RUVIA_CHECK(compressed.has_value());
    RUVIA_CHECK(compressed->compressed());
    RUVIA_CHECK(!decodeWebSocketFrameStart(0xC0, 0x80, true));
    RUVIA_CHECK(!decodeWebSocketFrameStart(0xC8, 0x80, true));
}

RUVIA_TEST(ws_control_frame_rules) {
    const auto close = decodeWebSocketFrameStart(0x88, 0x80, false);
    RUVIA_CHECK(!isInvalidWebSocketControlFrame(*close, 0));
    RUVIA_CHECK(!isInvalidWebSocketControlFrame(*close, 125));
    RUVIA_CHECK(isInvalidWebSocketControlFrame(*close, 126));
    const auto fragmentedPing = decodeWebSocketFrameStart(0x09, 0x80, false);
    RUVIA_CHECK(isInvalidWebSocketControlFrame(*fragmentedPing, 10));
    const auto data = decodeWebSocketFrameStart(0x01, 0x80, false);
    RUVIA_CHECK(!isInvalidWebSocketControlFrame(*data, 1000000));
}

RUVIA_TEST(ws_frame_view_factories_exclude_invalid_metadata_combinations) {
    const auto continuation = WebSocketFrameView::continuation("next", false);
    RUVIA_CHECK(continuation.kind() == WebSocketFrameKind::kContinuation);
    RUVIA_CHECK(!continuation.final());
    RUVIA_CHECK(!continuation.compressed());

    const auto compressedText = WebSocketFrameView::text("data", true, true);
    RUVIA_CHECK(compressedText.kind() == WebSocketFrameKind::kText);
    RUVIA_CHECK(compressedText.compressed());

    RUVIA_CHECK(WebSocketFrameView::ping("ok").has_value());
    const std::string oversizedPing(126, 'x');
    RUVIA_CHECK(!WebSocketFrameView::ping(oversizedPing).has_value());
    RUVIA_CHECK(WebSocketFrameView::close({}).has_value());
    RUVIA_CHECK(!WebSocketFrameView::close("x").has_value());
}

RUVIA_TEST(ws_encode_frame_header_length_boundaries) {
    WebSocketFrameHeader header{};
    const auto u8 = [&header](std::size_t index) {
        return static_cast<unsigned char>(header[index]);
    };
    // <=125: 1-byte length, no mask bit (server frames are unmasked).
    RUVIA_CHECK_EQ(encodeWebSocketFrameHeader(header, WebSocketOpcode::kText, 125), std::size_t{2});
    RUVIA_CHECK_EQ(u8(0), 0x81U);
    RUVIA_CHECK_EQ(u8(1), 125U);
    // 126..0xFFFF: 126 marker + 16-bit length.
    RUVIA_CHECK_EQ(encodeWebSocketFrameHeader(header, WebSocketOpcode::kBinary, 126), std::size_t{4});
    RUVIA_CHECK_EQ(u8(0), 0x82U);
    RUVIA_CHECK_EQ(u8(1), 126U);
    RUVIA_CHECK_EQ(u8(2), 0U);
    RUVIA_CHECK_EQ(u8(3), 126U);
    RUVIA_CHECK_EQ(encodeWebSocketFrameHeader(header, WebSocketOpcode::kText, 65535), std::size_t{4});
    // >0xFFFF: 127 marker + 64-bit length. 65536 = 0x0000000000010000.
    RUVIA_CHECK_EQ(encodeWebSocketFrameHeader(header, WebSocketOpcode::kText, 65536), std::size_t{10});
    RUVIA_CHECK_EQ(u8(1), 127U);
    RUVIA_CHECK_EQ(u8(2), 0U);
    RUVIA_CHECK_EQ(u8(7), 1U);  // the 0x10000 bit
    RUVIA_CHECK_EQ(u8(8), 0U);
    RUVIA_CHECK_EQ(u8(9), 0U);
}

RUVIA_TEST(ws_mask_unmask_round_trip_all_tail_sizes) {
    const char mask[4] = {0x12, 0x34, 0x56, 0x78};
    // XOR masking is involutive, so masking twice restores the payload; cover every
    // tail remainder (0..3) past the 4-byte-unrolled body.
    for (std::size_t n = 0; n <= 10; ++n) {
        std::string original(n, '\0');
        for (std::size_t i = 0; i < n; ++i) {
            original[i] = static_cast<char>('A' + static_cast<int>(i));
        }
        std::string buffer = original;
        decodeMaskedWebSocketPayload(buffer.data(), buffer.size(), mask);
        if (n > 0) {
            RUVIA_CHECK(buffer != original);
        }
        decodeMaskedWebSocketPayload(buffer.data(), buffer.size(), mask);
        RUVIA_CHECK_EQ(buffer, original);
    }
    // Known value: 'A' (0x41) ^ 0x12 == 0x53.
    std::string one("A");
    decodeMaskedWebSocketPayload(one.data(), one.size(), mask);
    RUVIA_CHECK_EQ(static_cast<unsigned char>(one[0]), 0x53U);
}

RUVIA_TEST(ws_frame_length_limit) {
    const auto limit = ProtocolByteLimit::limited(1000);
    RUVIA_CHECK(!webSocketFrameLengthExceedsLimit(100, limit));
    RUVIA_CHECK(webSocketFrameLengthExceedsLimit(1001, limit));
    RUVIA_CHECK(!webSocketFrameLengthExceedsLimit(
        1000000, ProtocolByteLimit::unlimited()));
    RUVIA_CHECK(webSocketFrameLengthExceedsLimit(
        std::numeric_limits<std::uint64_t>::max(), limit));
}

RUVIA_TEST(ws_frame_reader_needs_input_without_sentinel_metadata) {
    std::pmr::string input(std::pmr::get_default_resource());
    std::size_t offset = 0;
    std::size_t pendingCompactUntil = 0;

    const auto empty = webSocketTryReadFrame(
        input, offset, pendingCompactUntil,
        ProtocolByteLimit::limited(1024), false);
    RUVIA_CHECK(empty.needInput() != nullptr);
    RUVIA_CHECK(empty.frame() == nullptr);
    RUVIA_CHECK(empty.failure() == nullptr);

    input.push_back(static_cast<char>(0x81));
    const auto partial = webSocketTryReadFrame(
        input, offset, pendingCompactUntil,
        ProtocolByteLimit::limited(1024), false);
    RUVIA_CHECK(partial.needInput() != nullptr);
    RUVIA_CHECK(partial.frame() == nullptr);
    RUVIA_CHECK(partial.failure() == nullptr);
    RUVIA_CHECK_EQ(offset, std::size_t{0});
    RUVIA_CHECK_EQ(pendingCompactUntil, std::size_t{0});
}

RUVIA_TEST(ws_frame_reader_returns_one_unmasked_borrowed_frame) {
    auto input = maskedFrame(0x81, "hi");
    std::size_t offset = 0;
    std::size_t pendingCompactUntil = 0;

    const auto result = webSocketTryReadFrame(
        input, offset, pendingCompactUntil,
        ProtocolByteLimit::limited(1024), false);
    RUVIA_CHECK(result.needInput() == nullptr);
    RUVIA_CHECK(result.failure() == nullptr);
    RUVIA_CHECK(result.frame() != nullptr);
    RUVIA_CHECK(result.frame()->kind() == WebSocketFrameKind::kText);
    RUVIA_CHECK(result.frame()->final());
    RUVIA_CHECK(!result.frame()->compressed());
    RUVIA_CHECK_EQ(result.frame()->payload(), std::string_view("hi"));
    RUVIA_CHECK_EQ(offset, input.size());
    RUVIA_CHECK_EQ(pendingCompactUntil, input.size());
}

RUVIA_TEST(ws_frame_reader_reports_typed_wire_failures) {
    std::size_t offset = 0;
    std::size_t pendingCompactUntil = 0;
    std::pmr::string unmasked(
        std::string_view("\x81\x02hi", 4),
        std::pmr::get_default_resource());
    const auto maskFailure = webSocketTryReadFrame(
        unmasked, offset, pendingCompactUntil,
        ProtocolByteLimit::limited(1024), false);
    RUVIA_CHECK(maskFailure.failure() != nullptr);
    RUVIA_CHECK(
        maskFailure.failure()->error() ==
        WebSocketProtocolFailure::kProtocolError);
    RUVIA_CHECK(maskFailure.frame() == nullptr);

    auto tooLarge = maskedFrame(0x82, "123456");
    offset = 0;
    pendingCompactUntil = 0;
    const auto sizeFailure = webSocketTryReadFrame(
        tooLarge, offset, pendingCompactUntil,
        ProtocolByteLimit::limited(5), false);
    RUVIA_CHECK(sizeFailure.failure() != nullptr);
    RUVIA_CHECK(
        sizeFailure.failure()->error() ==
        WebSocketProtocolFailure::kMessageTooLarge);

    const std::string invalidClose(
        "\x03\xe8\xc0\x80", 4);  // 1000 plus invalid UTF-8 reason
    auto close = maskedFrame(0x88, invalidClose);
    offset = 0;
    pendingCompactUntil = 0;
    const auto closeFailure = webSocketTryReadFrame(
        close, offset, pendingCompactUntil,
        ProtocolByteLimit::limited(1024), false);
    RUVIA_CHECK(closeFailure.failure() != nullptr);
    RUVIA_CHECK(
        closeFailure.failure()->error() ==
        WebSocketProtocolFailure::kInvalidPayloadData);
}

// RFC 6455 §5.2: the length MUST be encoded in the minimal number of bytes. A frame
// using the 16-bit form for a length <126, or the 64-bit form for a length <=65535,
// is a protocol error. A conformant peer never emits these; reject them.
RUVIA_TEST(ws_frame_reader_rejects_non_minimal_length_encoding) {
    constexpr std::array<unsigned char, 4> mask{0x12, 0x34, 0x56, 0x78};
    const auto maskedBody = [&mask](std::pmr::string& out, std::string_view body) {
        for (const auto byte : mask) {
            out.push_back(static_cast<char>(byte));
        }
        for (std::size_t i = 0; i < body.size(); ++i) {
            out.push_back(static_cast<char>(
                static_cast<unsigned char>(body[i]) ^ mask[i & 3U]));
        }
    };
    const auto readsAsProtocolError = [](std::pmr::string& frame) {
        std::size_t offset = 0;
        std::size_t pendingCompactUntil = 0;
        const auto result = webSocketTryReadFrame(
            frame, offset, pendingCompactUntil,
            ProtocolByteLimit::limited(1U << 20), false);
        return result.failure() != nullptr &&
            result.failure()->error() == WebSocketProtocolFailure::kProtocolError;
    };

    // 16-bit form (126) carrying a 2-byte payload -- should have used the 7-bit form.
    std::pmr::string wide16(std::pmr::get_default_resource());
    wide16.push_back(static_cast<char>(0x82));       // FIN + binary
    wide16.push_back(static_cast<char>(0x80U | 126U));  // masked, 16-bit length marker
    wide16.push_back(0x00);
    wide16.push_back(0x02);                           // length = 2 (non-minimal)
    maskedBody(wide16, "hi");
    RUVIA_CHECK(readsAsProtocolError(wide16));

    // 64-bit form (127) carrying a 2-byte payload -- should have used the 7-bit form.
    std::pmr::string wide64(std::pmr::get_default_resource());
    wide64.push_back(static_cast<char>(0x82));
    wide64.push_back(static_cast<char>(0x80U | 127U));  // masked, 64-bit length marker
    for (int i = 0; i < 7; ++i) {
        wide64.push_back(0x00);
    }
    wide64.push_back(0x02);                            // length = 2 (non-minimal)
    maskedBody(wide64, "hi");
    RUVIA_CHECK(readsAsProtocolError(wide64));

    // Boundary: a genuinely 126-byte payload legitimately uses the 16-bit form.
    std::pmr::string minimal16(std::pmr::get_default_resource());
    minimal16.push_back(static_cast<char>(0x82));
    minimal16.push_back(static_cast<char>(0x80U | 126U));
    minimal16.push_back(0x00);
    minimal16.push_back(0x7E);                         // length = 126 (minimal)
    maskedBody(minimal16, std::string(126, 'x'));
    std::size_t offset = 0;
    std::size_t pendingCompactUntil = 0;
    const auto ok = webSocketTryReadFrame(
        minimal16, offset, pendingCompactUntil,
        ProtocolByteLimit::limited(1U << 20), false);
    RUVIA_CHECK(ok.frame() != nullptr);
    RUVIA_CHECK_EQ(ok.frame()->payload().size(), std::size_t{126});
}
