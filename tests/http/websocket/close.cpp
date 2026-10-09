#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "test_harness.h"
#include "websocket/http_websocket_close_payload.h"
#include "websocket/http_websocket_frame_codec.h"
#include "websocket/http_websocket_payload_validation.h"

namespace {

using ruvia::detail::encode_websocket_close_payload;
using ruvia::detail::websocket_close_payload_encode_error;
using ruvia::detail::websocket_close_payload_encode_result;
using ruvia::detail::websocket_close_payload_failure;
using ruvia::detail::websocket_encoded_close_payload;
using ruvia::detail::websocket_protocol_failure;
using ruvia::detail::websocket_protocol_failure_close_code;

std::string close_body(std::uint16_t code, std::string_view reason) {
    std::string body;
    body += static_cast<char>((code >> 8) & 0xFF);
    body += static_cast<char>(code & 0xFF);
    body += reason;
    return body;
}

std::optional<websocket_close_payload_encode_error> encode_error(
    std::uint16_t code, std::string_view reason) {
    const auto result_value = encode_websocket_close_payload(code, reason);
    const auto* failure = result_value.failure();
    return failure == nullptr ? std::nullopt : std::optional(failure->error());
}

// The RFC 6455 §7.4.1 close code carried by the rejection, or 0 if the body is
// accepted (used to pin that the read loop echoes the right code, not 1011).
std::uint16_t failure_close_code(std::string_view body) {
    const auto failure = websocket_close_payload_failure(body);
    return failure.has_value() ? websocket_protocol_failure_close_code(*failure) : 0;
}

}  // namespace

RUVIA_TEST(ws_close_encode_valid) {
    const auto result_value = encode_websocket_close_payload(1000, "bye");
    const auto* encoded = result_value.encoded();
    RUVIA_CHECK(encoded != nullptr);
    if (encoded != nullptr) {
        const auto payload_value = encoded->bytes();
        RUVIA_CHECK_EQ(payload_value.size(), std::size_t{5});                 // 2-byte code + "bye"
        RUVIA_CHECK_EQ(static_cast<unsigned char>(payload_value[0]), 0x03U);  // 1000 = 0x03E8
        RUVIA_CHECK_EQ(static_cast<unsigned char>(payload_value[1]), 0xE8U);
        RUVIA_CHECK_EQ(payload_value.substr(2), std::string_view("bye"));
    }
    // An empty reason encodes to just the 2-byte code.
    const auto empty = encode_websocket_close_payload(1001, "");
    RUVIA_CHECK(empty.encoded() != nullptr);
    if (empty.encoded() != nullptr) {
        RUVIA_CHECK_EQ(empty.encoded()->bytes().size(), std::size_t{2});
    }
}

RUVIA_TEST(ws_close_encode_rejects_invalid) {
    RUVIA_CHECK(encode_error(1005, "x") == websocket_close_payload_encode_error::invalid_code);
    RUVIA_CHECK(encode_error(1000, std::string("\xc0\x80", 2)) ==
                websocket_close_payload_encode_error::invalid_reason);
    RUVIA_CHECK(encode_error(1000, std::string(124, 'x')) ==
                websocket_close_payload_encode_error::reason_too_large);
    RUVIA_CHECK(!encode_error(1000, std::string(123, 'x')).has_value());
}

RUVIA_TEST(ws_close_validate_incoming) {
    // An empty close body is valid; a 1-byte body (a partial code) is not.
    RUVIA_CHECK(!websocket_close_payload_failure(std::string_view()).has_value());
    RUVIA_CHECK(websocket_close_payload_failure(std::string(1, 'x')) ==
                websocket_protocol_failure::protocol_error);
    // A valid code with a valid UTF-8 reason passes.
    RUVIA_CHECK(!websocket_close_payload_failure(close_body(1000, "ok")).has_value());
    // A reserved code or an invalid-UTF-8 reason is rejected.
    RUVIA_CHECK(websocket_close_payload_failure(close_body(1005, "")) ==
                websocket_protocol_failure::protocol_error);
    RUVIA_CHECK(websocket_close_payload_failure(close_body(1000, std::string("\xc0\x80", 2))) ==
                websocket_protocol_failure::invalid_payload_data);
}

RUVIA_TEST(ws_close_incoming_violation_carries_rfc_close_code) {
    // RFC 6455 §7.4.1: a malformed incoming Close is a protocol error (1002); a
    // Close whose 2-byte code is valid but whose reason is not UTF-8 is invalid
    // payload data (1007). The read loop echoes this code instead of a generic 1011.
    RUVIA_CHECK_EQ(
        failure_close_code(std::string(1, 'x')), std::uint16_t{1002});              // 1-byte partial code
    RUVIA_CHECK_EQ(failure_close_code(close_body(1005, "")), std::uint16_t{1002});  // reserved code
    RUVIA_CHECK_EQ(
        failure_close_code(close_body(1006, "")), std::uint16_t{1002});  // never-on-wire code
    RUVIA_CHECK_EQ(failure_close_code(close_body(1000, std::string("\xc0\x80", 2))),
        std::uint16_t{1007});                                                      // bad UTF-8 reason
    RUVIA_CHECK_EQ(failure_close_code(close_body(1000, "ok")), std::uint16_t{0});  // valid: accepted
}
