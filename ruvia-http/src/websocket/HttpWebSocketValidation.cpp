#include "ruvia/http/detail/websocket/HttpWebSocketClosePayload.h"
#include "ruvia/http/detail/websocket/HttpWebSocketFrameCodec.h"
#include "ruvia/http/detail/websocket/HttpWebSocketPayloadValidation.h"
#include "ruvia/http/detail/websocket/HttpWebSocketHandshakeFields.h"

#include <array>
#include <cstring>
#include <optional>

#include "ruvia/http/detail/HttpRequestAccess.h"
#include "ruvia/http/detail/HttpConnectionFields.h"
#include "ruvia/http/detail/HeaderTokenUtils.h"
#include "ruvia/http/detail/http1/Http1RequestBodyPlan.h"
#include "ruvia/http/detail/websocket/HttpWebSocketHandshakeValidation.h"
#include "ruvia/http/HttpRequest.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] bool webSocketHeaderEquals(std::string_view value, std::string_view expected) noexcept {
    return detail::httpAsciiEqualsIgnoreCase(detail::httpTrimOws(value), expected);
}

[[nodiscard]] std::optional<std::uint8_t> base64Value(char c) noexcept {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<std::uint8_t>(c - 'A');
    }
    if (c >= 'a' && c <= 'z') {
        return static_cast<std::uint8_t>(26 + c - 'a');
    }
    if (c >= '0' && c <= '9') {
        return static_cast<std::uint8_t>(52 + c - '0');
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::array<std::uint8_t, 16>> decodeWebSocketKey(std::string_view key) noexcept {
    key = detail::httpTrimOws(key);
    if (key.size() != 24) {
        return std::nullopt;
    }

    std::array<std::uint8_t, 16> nonce{};
    std::size_t out = 0;
    const auto emit = [&nonce, &out](std::uint8_t byte) noexcept {
        if (out < nonce.size()) {
            nonce[out] = byte;
        }
        ++out;
    };
    for (std::size_t i = 0; i < key.size(); i += 4) {
        std::array<std::uint8_t, 4> values{};
        std::size_t padding = 0;
        for (std::size_t j = 0; j < 4; ++j) {
            const auto ch = key[i + j];
            if (ch == '=') {
                values[j] = 0;
                ++padding;
                continue;
            }
            if (padding != 0) {
                return std::nullopt;
            }
            const auto value = base64Value(ch);
            if (!value) {
                return std::nullopt;
            }
            values[j] = *value;
        }
        if (padding > 2 || (padding != 0 && i + 4 != key.size())) {
            return std::nullopt;
        }
        // RFC 4648 requires unused bits in the final base64 quantum to be
        // zero.  Without this check, multiple non-canonical strings decode
        // to the same nonce and are incorrectly accepted as WebSocket keys.
        if ((padding == 2 && (values[1] & 0x0FU) != 0) ||
            (padding == 1 && (values[2] & 0x03U) != 0)) {
            return std::nullopt;
        }
        const auto triple =
            (static_cast<std::uint32_t>(values[0]) << 18) |
            (static_cast<std::uint32_t>(values[1]) << 12) |
            (static_cast<std::uint32_t>(values[2]) << 6) |
            static_cast<std::uint32_t>(values[3]);
        emit(static_cast<std::uint8_t>((triple >> 16) & 0xFF));
        if (padding < 2) {
            emit(static_cast<std::uint8_t>((triple >> 8) & 0xFF));
        }
        if (padding < 1) {
            emit(static_cast<std::uint8_t>(triple & 0xFF));
        }
    }
    if (out != 16) {
        return std::nullopt;
    }

    return nonce;
}

}  // namespace

HttpWebSocketHandshakeValidationResult validateHttp1WebSocketHandshake(
    const HttpRequest& request,
    const Http1RequestBodyPlan& bodyPlan) noexcept {
    HttpConnectionOptions connectionOptions;
    HttpUpgradeProtocols upgradeProtocols;
    std::string_view key;
    std::string_view version;
    std::size_t keyCount = 0;
    std::size_t versionCount = 0;
    bool webSocketUpgrade = false;

    for (const auto& header : request.headers()) {
        if (httpAsciiEqualsIgnoreCase(header.name(), "Connection")) {
            if (connectionOptions.parseField(
                    header.value(),
                    HttpFieldListRole::kRecipient) !=
                HttpFieldListParseStatus::kOk) {
                return HttpWebSocketHandshakeValidationResult::
                    makeInvalidRequest();
            }
        } else if (httpAsciiEqualsIgnoreCase(header.name(), "Upgrade")) {
            if (upgradeProtocols.parseField(
                    header.value(),
                    HttpFieldListRole::kRecipient,
                    [&webSocketUpgrade](
                        const HttpUpgradeProtocol& protocol) noexcept {
                        if (protocol.version.empty() &&
                            httpAsciiEqualsIgnoreCase(
                                protocol.name, "websocket")) {
                            webSocketUpgrade = true;
                        }
                        return true;
                    }) != HttpFieldListParseStatus::kOk) {
                return HttpWebSocketHandshakeValidationResult::
                    makeInvalidRequest();
            }
        } else if (httpAsciiEqualsIgnoreCase(
                       header.name(), "Sec-WebSocket-Key")) {
            key = header.value();
            ++keyCount;
        } else if (httpAsciiEqualsIgnoreCase(
                       header.name(), "Sec-WebSocket-Version")) {
            version = header.value();
            ++versionCount;
        }
    }

    if (request.knownMethod() != HttpKnownMethod::kGet ||
        request.protocolVersion() != HttpProtocolVersion::kHttp11 ||
        !connectionOptions.upgrade() ||
        !upgradeProtocols.hasProtocol() ||
        !webSocketUpgrade ||
        // RFC 6455 does not forbid Content-Length on the HTTP Upgrade request.
        // The parser-owned framing plan is the authoritative distinction:
        // Content-Length: 0 carries no content, while a positive length or
        // chunked coding still has bytes that must be consumed before the
        // connection can change protocols.
        bodyPlan.requiresConsumption() ||
        !webSocketSubprotocolOffersValid(request) ||
        !webSocketExtensionOffersValid(request) ||
        keyCount != 1 ||
        !decodeWebSocketKey(key).has_value() ||
        versionCount != 1) {
        return HttpWebSocketHandshakeValidationResult::makeInvalidRequest();
    }
    if (!webSocketHeaderEquals(version, "13")) {
        return HttpWebSocketHandshakeValidationResult::
            makeUnsupportedVersion();
    }
    return HttpWebSocketHandshakeValidationResult::makeAccepted();
}

bool isValidWebSocketCloseCode(std::uint16_t code) noexcept {
    if (code == 1000 || code == 1001 || code == 1002 || code == 1003 ||
        (code >= 1007 && code <= 1014)) {
        return true;
    }
    return code >= 3000 && code <= 4999;
}

bool isValidUtf8(std::string_view value) noexcept {
    std::uint32_t codepoint = 0;
    std::uint32_t remaining = 0;
    std::uint32_t minValue = 0;
    for (const auto ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        if (remaining == 0) {
            if (byte <= 0x7F) {
                continue;
            }
            if (byte >= 0xC2 && byte <= 0xDF) {
                codepoint = byte & 0x1FU;
                remaining = 1;
                minValue = 0x80;
            } else if (byte >= 0xE0 && byte <= 0xEF) {
                codepoint = byte & 0x0FU;
                remaining = 2;
                minValue = 0x800;
            } else if (byte >= 0xF0 && byte <= 0xF4) {
                codepoint = byte & 0x07U;
                remaining = 3;
                minValue = 0x10000;
            } else {
                return false;
            }
        } else {
            if ((byte & 0xC0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6) | (byte & 0x3FU);
            --remaining;
            if (remaining == 0 &&
                (codepoint < minValue || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))) {
                return false;
            }
        }
    }
    return remaining == 0;
}

WebSocketEncodedClosePayload::WebSocketEncodedClosePayload(
    std::uint16_t code,
    std::string_view reason) noexcept
    : size_(static_cast<std::uint8_t>(reason.size() + 2)) {
    bytes_[0] = static_cast<char>((code >> 8) & 0xFF);
    bytes_[1] = static_cast<char>(code & 0xFF);
    if (!reason.empty()) {
        std::memcpy(bytes_.data() + 2, reason.data(), reason.size());
    }
}

WebSocketClosePayloadEncodeResult encodeWebSocketClosePayload(
    std::uint16_t code,
    std::string_view reason) noexcept {
    if (!isValidWebSocketCloseCode(code)) {
        return WebSocketClosePayloadEncodeResult(
            WebSocketClosePayloadEncodeFailure(
                WebSocketClosePayloadEncodeError::kInvalidCode));
    }
    if (reason.size() > 123) {
        return WebSocketClosePayloadEncodeResult(
            WebSocketClosePayloadEncodeFailure(
                WebSocketClosePayloadEncodeError::kReasonTooLarge));
    }
    if (!isValidUtf8(reason)) {
        return WebSocketClosePayloadEncodeResult(
            WebSocketClosePayloadEncodeFailure(
                WebSocketClosePayloadEncodeError::kInvalidReason));
    }
    return WebSocketClosePayloadEncodeResult(
        WebSocketEncodedClosePayload(code, reason));
}

std::optional<WebSocketProtocolFailure>
webSocketClosePayloadFailure(std::string_view payload) noexcept {
    // Incoming Close frame (RFC 6455 §5.5.1). A malformed frame is a protocol
    // error (close 1002); an otherwise-valid frame whose reason is not valid
    // UTF-8 is invalid payload data (close 1007, §8.1).
    if (payload.size() == 1) {
        return WebSocketProtocolFailure::kProtocolError;
    }
    if (payload.size() < 2) {
        return std::nullopt;
    }
    const auto code = static_cast<std::uint16_t>(
        (static_cast<unsigned char>(payload[0]) << 8) |
        static_cast<unsigned char>(payload[1]));
    if (!isValidWebSocketCloseCode(code)) {
        return WebSocketProtocolFailure::kProtocolError;
    }
    if (!isValidUtf8(payload.substr(2))) {
        return WebSocketProtocolFailure::kInvalidPayloadData;
    }
    return std::nullopt;
}

}  // namespace ruvia::detail
