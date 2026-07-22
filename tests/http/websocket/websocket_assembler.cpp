#include "test_harness.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/detail/websocket/HttpWebSocketFrameCodec.h"
#include "ruvia/http/detail/websocket/HttpWebSocketFrameView.h"
#include "ruvia/http/detail/websocket/HttpWebSocketInboundAssembler.h"
#include "ruvia/http/WebSocketProtocol.h"

namespace {

using ruvia::ProtocolByteLimit;
using ruvia::WebSocketMessage;
using ruvia::WebSocketOpcode;
using ruvia::detail::WebSocketFrameView;
using ruvia::detail::WebSocketInboundAssembler;
using ruvia::detail::WebSocketInboundContentEncoding;
using ruvia::detail::WebSocketInboundResult;
using ruvia::detail::WebSocketMessageAccess;
using ruvia::detail::WebSocketProtocolFailure;
using ruvia::detail::webSocketProtocolFailureCloseCode;

template <typename Payload>
concept AcceptsWebSocketMessagePayload = requires(Payload&& payload) {
    WebSocketMessageAccess::make(
        WebSocketOpcode::kText, std::forward<Payload>(payload));
};

static_assert(!AcceptsWebSocketMessagePayload<std::string>);
static_assert(!AcceptsWebSocketMessagePayload<const std::string>);
static_assert(!AcceptsWebSocketMessagePayload<std::pmr::string>);
static_assert(AcceptsWebSocketMessagePayload<std::string&>);
static_assert(AcceptsWebSocketMessagePayload<std::pmr::string&>);
static_assert(AcceptsWebSocketMessagePayload<std::string_view>);

WebSocketFrameView frame(
    WebSocketOpcode opcode, std::string_view payload, bool fin,
    bool continuation = false, bool rsv1 = false) {
    if (continuation) {
        return WebSocketFrameView::continuation(payload, fin);
    }
    switch (opcode) {
        case WebSocketOpcode::kText:
            return WebSocketFrameView::text(payload, fin, rsv1);
        case WebSocketOpcode::kBinary:
            return WebSocketFrameView::binary(payload, fin, rsv1);
        case WebSocketOpcode::kClose:
            return *WebSocketFrameView::close(payload);
        case WebSocketOpcode::kPing:
            return *WebSocketFrameView::ping(payload);
        case WebSocketOpcode::kPong:
            return *WebSocketFrameView::pong(payload);
    }
    return WebSocketFrameView::text(payload, fin, rsv1);
}

ProtocolByteLimit byteLimit(std::size_t bytes) {
    return ProtocolByteLimit::limited(bytes);
}

// The RFC 6455 §7.4.1 close code the violation must be reported with (0 if none).
std::uint16_t acceptCloseCode(
    WebSocketInboundAssembler& assembler,
    const WebSocketFrameView& f,
    ProtocolByteLimit messageLimit) {
    const auto result = assembler.accept(f, messageLimit);
    const auto* failure = result.failure();
    return failure != nullptr
        ? webSocketProtocolFailureCloseCode(failure->error())
        : 0;
}

template <typename T>
concept HasInboundAction = requires(const T& result) {
    result.action();
};

template <typename T>
concept HasInboundError = requires(const T& result) {
    { result.error() } -> std::same_as<WebSocketProtocolFailure>;
};

template <typename T>
concept HasInboundOpcode = requires(const T& result) {
    { result.opcode() } -> std::same_as<WebSocketOpcode>;
};

template <typename T>
concept HasInboundContentEncoding = requires(const T& result) {
    { result.contentEncoding() } ->
        std::same_as<WebSocketInboundContentEncoding>;
};

template <typename T>
concept HasAnyRvalueInboundAccessor =
    requires(T&& result) { std::move(result).continueReading(); } ||
    requires(T&& result) { std::move(result).controlFrame(); } ||
    requires(T&& result) { std::move(result).message(); } ||
    requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept ExposesRvalueInboundMessageMember = requires(T&& message) {
    std::move(message).message();
};

static_assert(!std::default_initializable<WebSocketInboundResult>);
static_assert(std::same_as<
    decltype(std::declval<const WebSocketInboundResult&>().continueReading()),
    const ruvia::detail::WebSocketInboundContinue*>);
static_assert(std::same_as<
    decltype(std::declval<const WebSocketInboundResult&>().controlFrame()),
    const ruvia::detail::WebSocketInboundControlFrame*>);
static_assert(std::same_as<
    decltype(std::declval<const WebSocketInboundResult&>().message()),
    const ruvia::detail::WebSocketInboundMessage*>);
static_assert(std::same_as<
    decltype(std::declval<const WebSocketInboundResult&>().failure()),
    const ruvia::detail::WebSocketInboundFailure*>);
static_assert(!HasInboundAction<WebSocketInboundResult>);
static_assert(!HasInboundError<WebSocketInboundResult>);
static_assert(!HasAnyRvalueInboundAccessor<WebSocketInboundResult>);
static_assert(HasInboundOpcode<ruvia::detail::WebSocketInboundControlFrame>);
static_assert(!HasInboundContentEncoding<
    ruvia::detail::WebSocketInboundControlFrame>);
static_assert(!HasInboundOpcode<ruvia::detail::WebSocketInboundMessage>);
static_assert(HasInboundContentEncoding<
    ruvia::detail::WebSocketInboundMessage>);
static_assert(!HasInboundError<ruvia::detail::WebSocketInboundMessage>);
static_assert(!ExposesRvalueInboundMessageMember<
    ruvia::detail::WebSocketInboundMessage>);
static_assert(HasInboundError<ruvia::detail::WebSocketInboundFailure>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::WebSocketInboundFragmented&>().opcode()),
    WebSocketOpcode>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::WebSocketInboundFragmented&>().encoding()),
    WebSocketInboundContentEncoding>);

}  // namespace

RUVIA_TEST(ws_assembler_control_frames) {
    WebSocketInboundAssembler assembler(std::pmr::get_default_resource());
    const auto ping = assembler.accept(
        frame(WebSocketOpcode::kPing, "p", true), byteLimit(1000));
    RUVIA_CHECK(ping.controlFrame() != nullptr);
    RUVIA_CHECK(ping.controlFrame()->opcode() == WebSocketOpcode::kPing);
    RUVIA_CHECK_EQ(ping.controlFrame()->payload(), std::string_view("p"));

    const auto pong = assembler.accept(
        frame(WebSocketOpcode::kPong, "", true), byteLimit(1000));
    RUVIA_CHECK(pong.controlFrame() != nullptr);
    RUVIA_CHECK(pong.controlFrame()->opcode() == WebSocketOpcode::kPong);

    const auto close = assembler.accept(
        frame(WebSocketOpcode::kClose, "", true), byteLimit(1000));
    RUVIA_CHECK(close.controlFrame() != nullptr);
    RUVIA_CHECK(close.controlFrame()->opcode() == WebSocketOpcode::kClose);
}

RUVIA_TEST(ws_assembler_single_frame_messages) {
    WebSocketInboundAssembler assembler(std::pmr::get_default_resource());
    const auto text = assembler.accept(
        frame(WebSocketOpcode::kText, "hello", true), byteLimit(1000));
    RUVIA_CHECK(text.message() != nullptr);
    RUVIA_CHECK_EQ(
        text.message()->message().payload(), std::string_view("hello"));
    RUVIA_CHECK(
        text.message()->contentEncoding() ==
        WebSocketInboundContentEncoding::kIdentity);
    // Binary is delivered without UTF-8 checking.
    const std::string binary("\xff\xfe\x00\x01", 4);
    const auto binaryResult = assembler.accept(
        frame(WebSocketOpcode::kBinary, binary, true), byteLimit(1000));
    RUVIA_CHECK(binaryResult.message() != nullptr);
    RUVIA_CHECK_EQ(binaryResult.message()->message().payload(),
                   std::string_view(binary));
    // A compressed (RSV1) frame defers to the connection for inflation.
    const auto compressed = assembler.accept(
        frame(WebSocketOpcode::kText, "z", true, false, true), byteLimit(1000));
    RUVIA_CHECK(compressed.message() != nullptr);
    RUVIA_CHECK(
        compressed.message()->contentEncoding() ==
        WebSocketInboundContentEncoding::kPerMessageDeflate);
}

RUVIA_TEST(ws_assembler_invalid_utf8_text) {
    WebSocketInboundAssembler assembler(std::pmr::get_default_resource());
    const std::string overlong("\xc0\x80", 2);  // overlong encoding of NUL
    const auto result = assembler.accept(
        frame(WebSocketOpcode::kText, overlong, true), byteLimit(1000));
    RUVIA_CHECK(result.failure() != nullptr);
    RUVIA_CHECK(
        result.failure()->error() ==
        WebSocketProtocolFailure::kInvalidPayloadData);
}

RUVIA_TEST(ws_assembler_fragmented_message) {
    WebSocketInboundAssembler assembler(std::pmr::get_default_resource());
    const auto first = assembler.accept(
        frame(WebSocketOpcode::kText, "hel", false), byteLimit(1000));
    RUVIA_CHECK(first.continueReading() != nullptr);
    const auto second = assembler.accept(
        frame(WebSocketOpcode::kText, "lo ", false, true), byteLimit(1000));
    RUVIA_CHECK(second.continueReading() != nullptr);
    const auto complete = assembler.accept(
        frame(WebSocketOpcode::kText, "world", true, true), byteLimit(1000));
    RUVIA_CHECK(complete.message() != nullptr);
    RUVIA_CHECK_EQ(complete.message()->message().payload(),
                   std::string_view("hello world"));
}

RUVIA_TEST(ws_assembler_control_frame_interleaved_in_fragments) {
    // RFC 6455 §5.4: a control frame may be injected between the fragments of a
    // data message and MUST NOT disrupt the reassembly already in progress.
    WebSocketInboundAssembler assembler(std::pmr::get_default_resource());
    const auto first = assembler.accept(
        frame(WebSocketOpcode::kText, "hel", false), byteLimit(1000));
    RUVIA_CHECK(first.continueReading() != nullptr);
    // A ping arrives mid-message: it is answered but the fragment state is untouched.
    const auto ping = assembler.accept(
        frame(WebSocketOpcode::kPing, "p", true), byteLimit(1000));
    RUVIA_CHECK(ping.controlFrame() != nullptr);
    RUVIA_CHECK(ping.controlFrame()->opcode() == WebSocketOpcode::kPing);
    // The continuation still completes the ORIGINAL message intact.
    const auto complete = assembler.accept(
        frame(WebSocketOpcode::kText, "lo", true, true), byteLimit(1000));
    RUVIA_CHECK(complete.message() != nullptr);
    RUVIA_CHECK_EQ(complete.message()->message().payload(),
                   std::string_view("hello"));
}

RUVIA_TEST(ws_assembler_fragmented_compressed_defers_validation) {
    // A compressed message carries RSV1 on its FIRST frame only; the assembler must
    // remember that across continuation frames and, on completion, defer to the
    // connection for inflation (UTF-8 cannot be judged until the bytes are inflated).
    WebSocketInboundAssembler assembler(std::pmr::get_default_resource());
    const auto first = assembler.accept(
        frame(WebSocketOpcode::kText,
              std::string_view("\x01\x02", 2), false, false, true),
        byteLimit(1000));
    RUVIA_CHECK(first.continueReading() != nullptr);
    const auto complete = assembler.accept(
        frame(WebSocketOpcode::kText,
              std::string_view("\x03", 1), true, true),
        byteLimit(1000));
    RUVIA_CHECK(complete.message() != nullptr);
    RUVIA_CHECK(
        complete.message()->contentEncoding() ==
        WebSocketInboundContentEncoding::kPerMessageDeflate);
    RUVIA_CHECK_EQ(complete.message()->message().payload().size(),
                   std::size_t{3});
}

RUVIA_TEST(ws_assembler_protocol_errors) {
    // A continuation frame with no message in progress is a protocol violation.
    WebSocketInboundAssembler noStart(std::pmr::get_default_resource());
    const auto noStartResult = noStart.accept(
        frame(WebSocketOpcode::kText, "x", true, true), byteLimit(1000));
    RUVIA_CHECK(noStartResult.failure() != nullptr);
    RUVIA_CHECK(
        noStartResult.failure()->error() ==
        WebSocketProtocolFailure::kProtocolError);

    // A new data frame while a fragmented message is in progress is a violation.
    WebSocketInboundAssembler interleaved(std::pmr::get_default_resource());
    const auto started = interleaved.accept(
        frame(WebSocketOpcode::kText, "start", false), byteLimit(1000));
    RUVIA_CHECK(started.continueReading() != nullptr);
    const auto interleavedResult = interleaved.accept(
        frame(WebSocketOpcode::kText, "new", true), byteLimit(1000));
    RUVIA_CHECK(interleavedResult.failure() != nullptr);
    RUVIA_CHECK(
        interleavedResult.failure()->error() ==
        WebSocketProtocolFailure::kProtocolError);

    // Exceeding the per-message size limit across fragments is an explicit failure.
    WebSocketInboundAssembler tooBig(std::pmr::get_default_resource());
    const auto withinLimit = tooBig.accept(
        frame(WebSocketOpcode::kText, "12345", false), byteLimit(10));
    RUVIA_CHECK(withinLimit.continueReading() != nullptr);
    const auto tooBigResult = tooBig.accept(
        frame(WebSocketOpcode::kText, "678901", true, true), byteLimit(10));
    RUVIA_CHECK(tooBigResult.failure() != nullptr);
    RUVIA_CHECK(
        tooBigResult.failure()->error() ==
        WebSocketProtocolFailure::kMessageTooLarge);

    WebSocketInboundAssembler firstFrameTooBig(
        std::pmr::get_default_resource());
    const auto firstFrameResult = firstFrameTooBig.accept(
        frame(WebSocketOpcode::kBinary, "123456", false), byteLimit(5));
    RUVIA_CHECK(firstFrameResult.failure() != nullptr);
    RUVIA_CHECK(
        firstFrameResult.failure()->error() ==
        WebSocketProtocolFailure::kMessageTooLarge);
}

RUVIA_TEST(ws_assembler_violations_carry_rfc_close_code) {
    // RFC 6455 §7.4.1: framing/fragmentation violations report 1002 (protocol
    // error); a size-limit breach reports 1009 (message too big). The read loop
    // sends this code rather than the generic 1011 (internal error).
    WebSocketInboundAssembler noStart(std::pmr::get_default_resource());
    RUVIA_CHECK_EQ(
        acceptCloseCode(
            noStart,
            frame(WebSocketOpcode::kText, "x", true, true),
            byteLimit(1000)),
        std::uint16_t{1002});  // continuation with no message open

    WebSocketInboundAssembler interleaved(std::pmr::get_default_resource());
    (void)interleaved.accept(
        frame(WebSocketOpcode::kText, "start", false), byteLimit(1000));
    RUVIA_CHECK_EQ(
        acceptCloseCode(
            interleaved,
            frame(WebSocketOpcode::kText, "new", true),
            byteLimit(1000)),
        std::uint16_t{1002});  // interleaved non-continuation data frame

    WebSocketInboundAssembler tooBig(std::pmr::get_default_resource());
    (void)tooBig.accept(
        frame(WebSocketOpcode::kText, "12345", false), byteLimit(10));
    RUVIA_CHECK_EQ(
        acceptCloseCode(
            tooBig,
            frame(WebSocketOpcode::kText, "678901", true, true),
            byteLimit(10)),
        std::uint16_t{1009});  // per-message size limit exceeded
}
