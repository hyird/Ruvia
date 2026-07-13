#include "test_harness.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/detail/websocket/HttpWebSocketUtils.h"

namespace {

using ruvia::detail::encodeWebSocketClosePayload;
using ruvia::detail::WebSocketClosePayloadEncodeError;
using ruvia::detail::WebSocketProtocolFailure;
using ruvia::detail::webSocketClosePayloadFailure;
using ruvia::detail::webSocketProtocolFailureCloseCode;

std::string closeBody(std::uint16_t code, std::string_view reason) {
    std::string body;
    body += static_cast<char>((code >> 8) & 0xFF);
    body += static_cast<char>(code & 0xFF);
    body += reason;
    return body;
}

std::optional<WebSocketClosePayloadEncodeError> encodeError(
    std::uint16_t code,
    std::string_view reason) {
    const auto result = encodeWebSocketClosePayload(code, reason);
    const auto* failure = result.failure();
    return failure == nullptr
        ? std::nullopt
        : std::optional(failure->error());
}

// The RFC 6455 §7.4.1 close code carried by the rejection, or 0 if the body is
// accepted (used to pin that the read loop echoes the right code, not 1011).
std::uint16_t failureCloseCode(std::string_view body) {
    const auto failure = webSocketClosePayloadFailure(body);
    return failure.has_value()
        ? webSocketProtocolFailureCloseCode(*failure)
        : 0;
}

static_assert(noexcept(webSocketClosePayloadFailure(std::string_view{})));
static_assert(noexcept(encodeWebSocketClosePayload(
    std::uint16_t{}, std::string_view{})));

}  // namespace

RUVIA_TEST(ws_close_encode_valid) {
    const auto result = encodeWebSocketClosePayload(1000, "bye");
    const auto* encoded = result.encoded();
    RUVIA_CHECK(encoded != nullptr);
    if (encoded != nullptr) {
        const auto payload = encoded->bytes();
        RUVIA_CHECK_EQ(payload.size(), std::size_t{5});  // 2-byte code + "bye"
        RUVIA_CHECK_EQ(static_cast<unsigned char>(payload[0]), 0x03U);  // 1000 = 0x03E8
        RUVIA_CHECK_EQ(static_cast<unsigned char>(payload[1]), 0xE8U);
        RUVIA_CHECK_EQ(payload.substr(2), std::string_view("bye"));
    }
    // An empty reason encodes to just the 2-byte code.
    const auto empty = encodeWebSocketClosePayload(1001, "");
    RUVIA_CHECK(empty.encoded() != nullptr);
    if (empty.encoded() != nullptr) {
        RUVIA_CHECK_EQ(empty.encoded()->bytes().size(), std::size_t{2});
    }
}

RUVIA_TEST(ws_close_encode_rejects_invalid) {
    RUVIA_CHECK(encodeError(1005, "x") ==
        WebSocketClosePayloadEncodeError::kInvalidCode);
    RUVIA_CHECK(encodeError(1000, std::string("\xc0\x80", 2)) ==
        WebSocketClosePayloadEncodeError::kInvalidReason);
    RUVIA_CHECK(encodeError(1000, std::string(124, 'x')) ==
        WebSocketClosePayloadEncodeError::kReasonTooLarge);
    RUVIA_CHECK(!encodeError(1000, std::string(123, 'x')).has_value());
}

RUVIA_TEST(ws_close_validate_incoming) {
    // An empty close body is valid; a 1-byte body (a partial code) is not.
    RUVIA_CHECK(!webSocketClosePayloadFailure(std::string_view()).has_value());
    RUVIA_CHECK(
        webSocketClosePayloadFailure(std::string(1, 'x')) ==
        WebSocketProtocolFailure::kProtocolError);
    // A valid code with a valid UTF-8 reason passes.
    RUVIA_CHECK(!webSocketClosePayloadFailure(
        closeBody(1000, "ok")).has_value());
    // A reserved code or an invalid-UTF-8 reason is rejected.
    RUVIA_CHECK(
        webSocketClosePayloadFailure(closeBody(1005, "")) ==
        WebSocketProtocolFailure::kProtocolError);
    RUVIA_CHECK(
        webSocketClosePayloadFailure(
            closeBody(1000, std::string("\xc0\x80", 2))) ==
        WebSocketProtocolFailure::kInvalidPayloadData);
}

RUVIA_TEST(ws_close_incoming_violation_carries_rfc_close_code) {
    // RFC 6455 §7.4.1: a malformed incoming Close is a protocol error (1002); a
    // Close whose 2-byte code is valid but whose reason is not UTF-8 is invalid
    // payload data (1007). The read loop echoes this code instead of a generic 1011.
    RUVIA_CHECK_EQ(failureCloseCode(std::string(1, 'x')), std::uint16_t{1002});   // 1-byte partial code
    RUVIA_CHECK_EQ(failureCloseCode(closeBody(1005, "")), std::uint16_t{1002});   // reserved code
    RUVIA_CHECK_EQ(failureCloseCode(closeBody(1006, "")), std::uint16_t{1002});   // never-on-wire code
    RUVIA_CHECK_EQ(failureCloseCode(closeBody(1000, std::string("\xc0\x80", 2))),
                   std::uint16_t{1007});                                           // bad UTF-8 reason
    RUVIA_CHECK_EQ(failureCloseCode(closeBody(1000, "ok")), std::uint16_t{0});    // valid: accepted
}
