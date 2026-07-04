#include "test_harness.h"

#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>

#include "net/ws/HttpWebSocketUtils.h"
#include "ruvia/http/WebSocket.h"

namespace {

using ruvia::WebSocketMessage;
using ruvia::WebSocketOpcode;
using ruvia::detail::WebSocketFrameView;
using ruvia::detail::WebSocketInboundAction;
using ruvia::detail::WebSocketInboundAssembler;
using ruvia::detail::WebSocketMessageAccess;

WebSocketFrameView frame(
    WebSocketOpcode opcode, std::string_view payload, bool fin,
    bool continuation = false, bool rsv1 = false) {
    return WebSocketFrameView{
        .opcode = opcode, .payload = payload, .fin = fin,
        .continuation = continuation, .rsv1 = rsv1};
}

bool acceptThrows(WebSocketInboundAssembler& assembler, const WebSocketFrameView& f, std::size_t maxBytes) {
    auto out = WebSocketMessageAccess::make(WebSocketOpcode::kText, {});
    try {
        (void)assembler.accept(f, maxBytes, out);
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

RUVIA_TEST(ws_assembler_control_frames) {
    WebSocketInboundAssembler assembler(std::pmr::get_default_resource());
    auto out = WebSocketMessageAccess::make(WebSocketOpcode::kText, {});
    RUVIA_CHECK(assembler.accept(frame(WebSocketOpcode::kPing, "p", true), 1000, out) ==
                WebSocketInboundAction::kSendPong);
    RUVIA_CHECK(assembler.accept(frame(WebSocketOpcode::kPong, "", true), 1000, out) ==
                WebSocketInboundAction::kPongReceived);
    RUVIA_CHECK(assembler.accept(frame(WebSocketOpcode::kClose, "", true), 1000, out) ==
                WebSocketInboundAction::kPeerClose);
}

RUVIA_TEST(ws_assembler_single_frame_messages) {
    WebSocketInboundAssembler assembler(std::pmr::get_default_resource());
    auto out = WebSocketMessageAccess::make(WebSocketOpcode::kText, {});
    RUVIA_CHECK(assembler.accept(frame(WebSocketOpcode::kText, "hello", true), 1000, out) ==
                WebSocketInboundAction::kDeliver);
    RUVIA_CHECK_EQ(out.payload(), std::string_view("hello"));
    // Binary is delivered without UTF-8 checking.
    const std::string binary("\xff\xfe\x00\x01", 4);
    RUVIA_CHECK(assembler.accept(frame(WebSocketOpcode::kBinary, binary, true), 1000, out) ==
                WebSocketInboundAction::kDeliver);
    // A compressed (RSV1) frame defers to the connection for inflation.
    RUVIA_CHECK(assembler.accept(frame(WebSocketOpcode::kText, "z", true, false, true), 1000, out) ==
                WebSocketInboundAction::kDeliverCompressed);
}

RUVIA_TEST(ws_assembler_invalid_utf8_text) {
    WebSocketInboundAssembler assembler(std::pmr::get_default_resource());
    auto out = WebSocketMessageAccess::make(WebSocketOpcode::kText, {});
    const std::string overlong("\xc0\x80", 2);  // overlong encoding of NUL
    RUVIA_CHECK(assembler.accept(frame(WebSocketOpcode::kText, overlong, true), 1000, out) ==
                WebSocketInboundAction::kInvalidUtf8);
}

RUVIA_TEST(ws_assembler_fragmented_message) {
    WebSocketInboundAssembler assembler(std::pmr::get_default_resource());
    auto out = WebSocketMessageAccess::make(WebSocketOpcode::kText, {});
    RUVIA_CHECK(assembler.accept(frame(WebSocketOpcode::kText, "hel", false), 1000, out) ==
                WebSocketInboundAction::kContinue);
    RUVIA_CHECK(assembler.accept(frame(WebSocketOpcode::kText, "lo ", false, true), 1000, out) ==
                WebSocketInboundAction::kContinue);
    RUVIA_CHECK(assembler.accept(frame(WebSocketOpcode::kText, "world", true, true), 1000, out) ==
                WebSocketInboundAction::kDeliver);
    RUVIA_CHECK_EQ(out.payload(), std::string_view("hello world"));
}

RUVIA_TEST(ws_assembler_protocol_errors) {
    // A continuation frame with no message in progress is a protocol violation.
    WebSocketInboundAssembler noStart(std::pmr::get_default_resource());
    RUVIA_CHECK(acceptThrows(noStart, frame(WebSocketOpcode::kText, "x", true, true), 1000));

    // A new data frame while a fragmented message is in progress is a violation.
    WebSocketInboundAssembler interleaved(std::pmr::get_default_resource());
    auto out = WebSocketMessageAccess::make(WebSocketOpcode::kText, {});
    RUVIA_CHECK(interleaved.accept(frame(WebSocketOpcode::kText, "start", false), 1000, out) ==
                WebSocketInboundAction::kContinue);
    RUVIA_CHECK(acceptThrows(interleaved, frame(WebSocketOpcode::kText, "new", true), 1000));

    // Exceeding the per-message size limit across fragments throws.
    WebSocketInboundAssembler tooBig(std::pmr::get_default_resource());
    RUVIA_CHECK(tooBig.accept(frame(WebSocketOpcode::kText, "12345", false), 10, out) ==
                WebSocketInboundAction::kContinue);
    RUVIA_CHECK(acceptThrows(tooBig, frame(WebSocketOpcode::kText, "678901", true, true), 10));
}
