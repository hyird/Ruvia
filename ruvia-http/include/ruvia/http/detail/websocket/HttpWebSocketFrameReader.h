#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/detail/websocket/HttpWebSocketFrameCodec.h"
#include "ruvia/http/detail/websocket/HttpWebSocketFrameView.h"
#include "ruvia/http/detail/websocket/HttpWebSocketPayloadValidation.h"

namespace ruvia::detail {

// Drop the bytes a completed frame consumed, compacting the buffer only once the
// dead prefix is worth the move.
inline void compactWebSocketReadBuffer(
    std::pmr::string& buffer,
    std::size_t& offset,
    std::size_t& pendingCompactUntil) noexcept {
    if (pendingCompactUntil == 0) {
        return;
    }

    const auto consumed = std::exchange(pendingCompactUntil, 0);
    if (consumed >= buffer.size()) {
        buffer.clear();
        offset = 0;
        return;
    }

    offset = consumed;
    constexpr std::size_t kCompactThresholdBytes = 16 * 1024;
    if (consumed < kCompactThresholdBytes && consumed < buffer.size() - consumed) {
        return;
    }

    const auto remaining = buffer.size() - consumed;
    std::memmove(buffer.data(), buffer.data() + consumed, remaining);
    buffer.resize(remaining);
    offset = 0;
}

class WebSocketFrameReadResult;

class WebSocketFrameNeedInput final {
private:
    friend class WebSocketFrameReadResult;
    constexpr WebSocketFrameNeedInput() noexcept = default;
};

class WebSocketFrameReadFailure final {
public:
    [[nodiscard]] constexpr WebSocketProtocolFailure error() const noexcept {
        return error_;
    }

private:
    friend class WebSocketFrameReadResult;

    explicit constexpr WebSocketFrameReadFailure(
        WebSocketProtocolFailure error) noexcept
        : error_(error) {}

    WebSocketProtocolFailure error_;
};

// Parsing either needs more input, exposes one complete borrowed frame, or reports
// one typed protocol failure. No default frame, byte-count hint, EOF boolean, or
// wire-format exception can coexist with another outcome.
class WebSocketFrameReadResult final {
public:
    [[nodiscard]] constexpr const WebSocketFrameNeedInput*
    needInput() const & noexcept {
        return std::get_if<WebSocketFrameNeedInput>(&value_);
    }
    [[nodiscard]] constexpr const WebSocketFrameNeedInput*
    needInput() const && = delete;

    [[nodiscard]] constexpr const WebSocketFrameView*
    frame() const & noexcept {
        return std::get_if<WebSocketFrameView>(&value_);
    }
    [[nodiscard]] constexpr const WebSocketFrameView*
    frame() const && = delete;

    [[nodiscard]] constexpr const WebSocketFrameReadFailure*
    failure() const & noexcept {
        return std::get_if<WebSocketFrameReadFailure>(&value_);
    }
    [[nodiscard]] constexpr const WebSocketFrameReadFailure*
    failure() const && = delete;

private:
    friend WebSocketFrameReadResult webSocketTryReadFrame(
        std::pmr::string&,
        std::size_t&,
        std::size_t&,
        ProtocolByteLimit,
        bool);

    using Value = std::variant<
        WebSocketFrameNeedInput,
        WebSocketFrameView,
        WebSocketFrameReadFailure>;

    template <typename Alternative>
    explicit constexpr WebSocketFrameReadResult(
        Alternative alternative) noexcept
        : value_(alternative) {}

    [[nodiscard]] static constexpr WebSocketFrameReadResult
    makeNeedInput() noexcept {
        return WebSocketFrameReadResult(WebSocketFrameNeedInput());
    }

    [[nodiscard]] static constexpr WebSocketFrameReadResult
    makeFrame(
        const WebSocketFrameStart& start,
        std::string_view payload) noexcept {
        return WebSocketFrameReadResult(WebSocketFrameView(start, payload));
    }

    [[nodiscard]] static constexpr WebSocketFrameReadResult
    makeFailure(WebSocketProtocolFailure error) noexcept {
        return WebSocketFrameReadResult(WebSocketFrameReadFailure(error));
    }

    Value value_;
};

// Single owner of WebSocket frame decode (RFC 6455 §5.2): FIN/opcode/length
// parsing, control-frame and length-limit validation, and in-place unmasking.
// It never performs I/O and never throws for peer bytes; callers append transport
// input after needInput(), while failure() carries the Close reason.
[[nodiscard]] inline WebSocketFrameReadResult webSocketTryReadFrame(
    std::pmr::string& buffer,
    std::size_t& offset,
    std::size_t& pendingCompactUntil,
    ProtocolByteLimit messageLimit,
    bool permessageDeflate) {
    compactWebSocketReadBuffer(buffer, offset, pendingCompactUntil);
    const auto available = buffer.size() - offset;
    if (available < 2) {
        return WebSocketFrameReadResult::makeNeedInput();
    }
    const auto first = static_cast<unsigned char>(buffer[offset]);
    const auto second = static_cast<unsigned char>(buffer[offset + 1]);
    std::uint64_t length = second & 0x7FU;
    std::size_t headerSize = 2;

    const auto frameStart =
        decodeWebSocketFrameStart(first, second, permessageDeflate);
    if (!frameStart.has_value()) {
        return WebSocketFrameReadResult::makeFailure(
            WebSocketProtocolFailure::kProtocolError);
    }
    if (length == 126) {
        if (available < headerSize + 2) {
            return WebSocketFrameReadResult::makeNeedInput();
        }
        length = readWebSocketUint16(buffer.data() + offset + headerSize);
        headerSize += 2;
        // RFC 6455 §5.2: the minimal number of length bytes MUST be used, so a
        // value <126 may not use the 16-bit form (e.g. 126,0,124 for a 124-byte
        // payload). A conformant peer never emits this; reject the non-minimal frame.
        if (length < 126) {
            return WebSocketFrameReadResult::makeFailure(
                WebSocketProtocolFailure::kProtocolError);
        }
    } else if (length == 127) {
        if (available < headerSize + 8) {
            return WebSocketFrameReadResult::makeNeedInput();
        }
        if (!readWebSocketUint64(buffer.data() + offset + headerSize, length)) {
            return WebSocketFrameReadResult::makeFailure(
                WebSocketProtocolFailure::kProtocolError);
        }
        headerSize += 8;
        // RFC 6455 §5.2 minimal-length rule: a value fitting the 16-bit form may
        // not use the 64-bit form.
        if (length <= 0xFFFFU) {
            return WebSocketFrameReadResult::makeFailure(
                WebSocketProtocolFailure::kProtocolError);
        }
    }

    if (isInvalidWebSocketControlFrame(*frameStart, length)) {
        return WebSocketFrameReadResult::makeFailure(
            WebSocketProtocolFailure::kProtocolError);
    }
    if (webSocketFrameExceedsMessageLimit(
            frameStart->kind(), length, messageLimit)) {
        return WebSocketFrameReadResult::makeFailure(
            WebSocketProtocolFailure::kMessageTooLarge);
    }
    if (webSocketMaskedFrameReadSizeOverflows(length, headerSize)) {
        return WebSocketFrameReadResult::makeFailure(
            WebSocketProtocolFailure::kProtocolError);
    }
    const auto totalFrameBytes = headerSize + 4 + static_cast<std::size_t>(length);
    if (available < totalFrameBytes) {
        return WebSocketFrameReadResult::makeNeedInput();
    }

    const auto maskOffset = offset + headerSize;
    const auto payloadOffset = maskOffset + 4;
    const auto payloadSize = static_cast<std::size_t>(length);
    auto* payload = buffer.data() + payloadOffset;
    decodeMaskedWebSocketPayload(payload, payloadSize, buffer.data() + maskOffset);
    const auto payloadView = std::string_view(payload, payloadSize);
    if (frameStart->kind() == WebSocketFrameKind::kClose) {
        if (const auto failure = webSocketClosePayloadFailure(payloadView)) {
            return WebSocketFrameReadResult::makeFailure(*failure);
        }
    }
    offset = payloadOffset + payloadSize;
    pendingCompactUntil = offset;
    return WebSocketFrameReadResult::makeFrame(*frameStart, payloadView);
}
}  // namespace ruvia::detail
