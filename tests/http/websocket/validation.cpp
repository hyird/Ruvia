#include "test_harness.h"

#include <cstdint>
#include <initializer_list>
#include <string>

#include "ruvia/http/detail/websocket/frame/HttpWebSocketPayloadValidation.h"

namespace {

using ruvia::detail::isValidUtf8;
using ruvia::detail::isValidWebSocketCloseCode;

// Build a byte string without hex-escape pitfalls.
std::string bytes(std::initializer_list<int> values) {
    std::string out;
    out.reserve(values.size());
    for (const int value : values) {
        out.push_back(static_cast<char>(value));
    }
    return out;
}

bool closeCodeValid(int code) {
    return isValidWebSocketCloseCode(static_cast<std::uint16_t>(code));
}

}  // namespace

// RFC 6455 §8.1 requires text-frame payloads to be valid UTF-8.
RUVIA_TEST(utf8_accepts_valid_sequences) {
    RUVIA_CHECK(isValidUtf8(""));
    RUVIA_CHECK(isValidUtf8("plain ASCII text"));
    RUVIA_CHECK(isValidUtf8(bytes({0xC2, 0xA9})));              // U+00A9 (c)
    RUVIA_CHECK(isValidUtf8(bytes({0xE2, 0x82, 0xAC})));        // U+20AC EUR
    RUVIA_CHECK(isValidUtf8(bytes({0xF0, 0x9F, 0x98, 0x80})));  // U+1F600 emoji
    RUVIA_CHECK(isValidUtf8(bytes({0xED, 0x9F, 0xBF})));        // U+D7FF, just below surrogates
    RUVIA_CHECK(isValidUtf8(bytes({0xEE, 0x80, 0x80})));        // U+E000, just above surrogates
    RUVIA_CHECK(isValidUtf8(bytes({0xF4, 0x8F, 0xBF, 0xBF})));  // U+10FFFF, the maximum
}

RUVIA_TEST(utf8_rejects_invalid_sequences) {
    RUVIA_CHECK(!isValidUtf8(bytes({0x80})));                    // stray continuation byte
    RUVIA_CHECK(!isValidUtf8(bytes({0xFF})));                    // invalid lead byte
    RUVIA_CHECK(!isValidUtf8(bytes({0xC0, 0x80})));              // overlong NUL (C0 lead)
    RUVIA_CHECK(!isValidUtf8(bytes({0xC1, 0xBF})));              // overlong (C1 lead)
    RUVIA_CHECK(!isValidUtf8(bytes({0xE0, 0x80, 0x80})));        // overlong 3-byte
    RUVIA_CHECK(!isValidUtf8(bytes({0xE0, 0x9F, 0xBF})));        // overlong encoding of U+07FF
    RUVIA_CHECK(!isValidUtf8(bytes({0xED, 0xA0, 0x80})));        // surrogate U+D800
    RUVIA_CHECK(!isValidUtf8(bytes({0xED, 0xBF, 0xBF})));        // surrogate U+DFFF
    RUVIA_CHECK(!isValidUtf8(bytes({0xF0, 0x80, 0x80, 0x80})));  // overlong 4-byte
    RUVIA_CHECK(!isValidUtf8(bytes({0xF4, 0x90, 0x80, 0x80})));  // U+110000, past the maximum
    RUVIA_CHECK(!isValidUtf8(bytes({0xF5, 0x80, 0x80, 0x80})));  // F5 lead, out of range
    RUVIA_CHECK(!isValidUtf8(bytes({0xC2})));                    // truncated 2-byte
    RUVIA_CHECK(!isValidUtf8(bytes({0xE2, 0x82})));              // truncated 3-byte
    RUVIA_CHECK(!isValidUtf8(bytes({0xC2, 0xC2})));              // lead byte where a continuation was due
}

RUVIA_TEST(websocket_close_code_validity) {
    // RFC 6455 §7.4.1 plus the IANA-registered 1012-1014
    // (service restart / try again later / bad gateway).
    for (const int code : {1000, 1001, 1002, 1003, 1007, 1008, 1009, 1010, 1011, 1012, 1013, 1014}) {
        RUVIA_CHECK(closeCodeValid(code));
    }
    RUVIA_CHECK(closeCodeValid(3000));  // registered range
    RUVIA_CHECK(closeCodeValid(4999));  // private range
    // Reserved or out-of-range codes must be rejected.
    for (const int code : {0, 999, 1004, 1005, 1006, 1015, 1016, 2999, 5000, 65535}) {
        RUVIA_CHECK(!closeCodeValid(code));
    }
}
