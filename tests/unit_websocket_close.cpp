#include "test_harness.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/detail/websocket/HttpWebSocketUtils.h"

namespace {

using ruvia::detail::encodeWebSocketClosePayload;
using ruvia::detail::validateWebSocketClosePayload;
using ruvia::detail::WebSocketClosePayload;
using ruvia::detail::WebSocketProtocolError;

std::string closeBody(std::uint16_t code, std::string_view reason) {
    std::string body;
    body += static_cast<char>((code >> 8) & 0xFF);
    body += static_cast<char>(code & 0xFF);
    body += reason;
    return body;
}

bool encodeThrows(std::uint16_t code, std::string_view reason) {
    WebSocketClosePayload payload;
    try {
        (void)encodeWebSocketClosePayload(payload, code, reason);
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

bool validateThrows(std::string_view body) {
    try {
        validateWebSocketClosePayload(body);
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

// The RFC 6455 §7.4.1 close code carried by the rejection, or 0 if the body is
// accepted (used to pin that the read loop echoes the right code, not 1011).
std::uint16_t validateCloseCode(std::string_view body) {
    try {
        validateWebSocketClosePayload(body);
        return 0;
    } catch (const WebSocketProtocolError& error) {
        return error.closeCode();
    }
}

}  // namespace

RUVIA_TEST(ws_close_encode_valid) {
    WebSocketClosePayload payload;
    const auto size = encodeWebSocketClosePayload(payload, 1000, "bye");
    RUVIA_CHECK_EQ(size, std::size_t{5});  // 2-byte code + "bye"
    RUVIA_CHECK_EQ(static_cast<unsigned char>(payload[0]), 0x03U);  // 1000 = 0x03E8
    RUVIA_CHECK_EQ(static_cast<unsigned char>(payload[1]), 0xE8U);
    RUVIA_CHECK_EQ(std::string_view(payload.data() + 2, 3), std::string_view("bye"));
    // An empty reason encodes to just the 2-byte code.
    RUVIA_CHECK_EQ(encodeWebSocketClosePayload(payload, 1001, ""), std::size_t{2});
}

RUVIA_TEST(ws_close_encode_rejects_invalid) {
    RUVIA_CHECK(encodeThrows(1005, "x"));                    // 1005 is a reserved code
    RUVIA_CHECK(encodeThrows(1000, std::string("\xc0\x80", 2)));  // reason is not valid UTF-8
    RUVIA_CHECK(encodeThrows(1000, std::string(124, 'x')));  // reason exceeds 123 bytes
    RUVIA_CHECK(!encodeThrows(1000, std::string(123, 'x')));  // exactly 123 is allowed
}

RUVIA_TEST(ws_close_validate_incoming) {
    // An empty close body is valid; a 1-byte body (a partial code) is not.
    RUVIA_CHECK(!validateThrows(std::string_view()));
    RUVIA_CHECK(validateThrows(std::string(1, 'x')));
    // A valid code with a valid UTF-8 reason passes.
    RUVIA_CHECK(!validateThrows(closeBody(1000, "ok")));
    // A reserved code or an invalid-UTF-8 reason is rejected.
    RUVIA_CHECK(validateThrows(closeBody(1005, "")));
    RUVIA_CHECK(validateThrows(closeBody(1000, std::string("\xc0\x80", 2))));
}

RUVIA_TEST(ws_close_incoming_violation_carries_rfc_close_code) {
    // RFC 6455 §7.4.1: a malformed incoming Close is a protocol error (1002); a
    // Close whose 2-byte code is valid but whose reason is not UTF-8 is invalid
    // payload data (1007). The read loop echoes this code instead of a generic 1011.
    RUVIA_CHECK_EQ(validateCloseCode(std::string(1, 'x')), std::uint16_t{1002});   // 1-byte partial code
    RUVIA_CHECK_EQ(validateCloseCode(closeBody(1005, "")), std::uint16_t{1002});   // reserved code
    RUVIA_CHECK_EQ(validateCloseCode(closeBody(1006, "")), std::uint16_t{1002});   // never-on-wire code
    RUVIA_CHECK_EQ(validateCloseCode(closeBody(1000, std::string("\xc0\x80", 2))),
                   std::uint16_t{1007});                                           // bad UTF-8 reason
    RUVIA_CHECK_EQ(validateCloseCode(closeBody(1000, "ok")), std::uint16_t{0});    // valid: accepted
}
