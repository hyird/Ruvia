#include "test_harness.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "net/ws/HttpWebSocketUtils.h"

namespace {

using ruvia::detail::encodeWebSocketClosePayload;
using ruvia::detail::validateWebSocketClosePayload;
using ruvia::detail::WebSocketClosePayload;

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
