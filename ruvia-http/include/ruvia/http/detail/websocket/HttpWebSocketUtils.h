#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/detail/BorrowedView.h"
#include "ruvia/http/detail/websocket/HttpWebSocketMessageAccess.h"
#include "ruvia/http/WebSocketProtocol.h"

namespace ruvia {

class HttpRequest;

namespace detail {

using WebSocketFrameHeader = std::array<char, 10>;
using WebSocketAcceptKey = std::array<char, 28>;

// Wire failures are protocol values, not exceptions. The numeric values are the
// RFC 6455 §7.4.1 Close codes the connection must send before ending transport.
enum class WebSocketProtocolFailure : std::uint16_t {
    kProtocolError = 1002,
    kInvalidPayloadData = 1007,
    kMessageTooLarge = 1009,
};

[[nodiscard]] constexpr std::uint16_t webSocketProtocolFailureCloseCode(
    WebSocketProtocolFailure failure) noexcept {
    return static_cast<std::uint16_t>(failure);
}

enum class WebSocketFrameKind : std::uint8_t {
    kContinuation = 0x0,
    kText = 0x1,
    kBinary = 0x2,
    kClose = 0x8,
    kPing = 0x9,
    kPong = 0xA,
};

// Validated first-byte semantics. The raw continuation opcode is preserved as
// its own kind instead of being normalized to Text plus an independent boolean;
// compression is likewise admitted only where RFC 7692 permits RSV1.
class WebSocketFrameStart final {
public:
    [[nodiscard]] constexpr WebSocketFrameKind kind() const noexcept {
        return kind_;
    }

    [[nodiscard]] constexpr bool final() const noexcept {
        return final_;
    }

    [[nodiscard]] constexpr bool compressed() const noexcept {
        return compressed_;
    }

private:
    friend std::optional<WebSocketFrameStart> decodeWebSocketFrameStart(
        unsigned char,
        unsigned char,
        bool) noexcept;

    constexpr WebSocketFrameStart(
        WebSocketFrameKind kind,
        bool final,
        bool compressed) noexcept
        : kind_(kind), final_(final), compressed_(compressed) {}

    WebSocketFrameKind kind_;
    bool final_;
    bool compressed_;
};

[[nodiscard]] inline bool isInvalidWebSocketRawOpcode(std::uint8_t rawOpcode) noexcept {
    return (rawOpcode >= 0x3 && rawOpcode <= 0x7) || rawOpcode >= 0xB;
}

[[nodiscard]] inline bool isWebSocketControlOpcode(WebSocketOpcode opcode) noexcept {
    return std::to_underlying(opcode) >= 0x8;
}

[[nodiscard]] inline bool isWebSocketControlFrameKind(
    WebSocketFrameKind kind) noexcept {
    return std::to_underlying(kind) >= 0x8;
}

// allowRsv1 enables the RSV1 (compressed) bit when permessage-deflate is
// negotiated; it is valid only on the first frame of a data message, never on a
// continuation or control frame (RFC 7692 §6.1). RSV2/RSV3 are always rejected.
[[nodiscard]] inline std::optional<WebSocketFrameStart>
decodeWebSocketFrameStart(
    unsigned char first,
    unsigned char second,
    bool allowRsv1) noexcept {
    const auto rawOpcode = static_cast<std::uint8_t>(first & 0x0FU);
    const bool rsv1 = (first & 0x40U) != 0;
    if ((first & 0x30U) != 0 || (second & 0x80U) == 0 || isInvalidWebSocketRawOpcode(rawOpcode)) {
        return std::nullopt;
    }
    if (rsv1 && (!allowRsv1 || rawOpcode == 0 || rawOpcode >= 0x8)) {
        return std::nullopt;
    }
    return WebSocketFrameStart(
        static_cast<WebSocketFrameKind>(rawOpcode),
        (first & 0x80U) != 0,
        rsv1);
}

[[nodiscard]] inline bool isInvalidWebSocketControlFrame(
    const WebSocketFrameStart& frame,
    std::uint64_t payloadSize) noexcept {
    return isWebSocketControlFrameKind(frame.kind()) &&
        (!frame.final() || payloadSize > 125);
}

[[nodiscard]] inline bool webSocketMessageExceedsLimit(
    std::size_t payloadSize,
    ProtocolByteLimit messageLimit) noexcept {
    return messageLimit.exceeds(payloadSize);
}

[[nodiscard]] inline bool webSocketAppendExceedsLimit(
    std::size_t currentSize,
    std::size_t appendSize,
    ProtocolByteLimit messageLimit) noexcept {
    return messageLimit.additionExceeds(currentSize, appendSize);
}

[[nodiscard]] inline bool webSocketFrameLengthExceedsLimit(
    std::uint64_t payloadSize,
    ProtocolByteLimit messageLimit) noexcept {
    if (payloadSize > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
        return true;
    }
    return messageLimit.exceeds(static_cast<std::size_t>(payloadSize));
}

// A control frame (Close/Ping/Pong) is capped at 125 bytes by RFC 6455 §5.5 and
// is explicitly NOT subject to the per-message size limit, so only data frames
// (Text/Binary/Continuation) are measured against maxMessageBytes. Applying the
// limit to control frames would reject a legal Ping/Pong, or a Close carrying a
// reason phrase, once maxMessageBytes drops below 125 -- silently breaking the
// close handshake and keepalive on a small-message configuration.
[[nodiscard]] inline bool webSocketFrameExceedsMessageLimit(
    WebSocketFrameKind kind,
    std::uint64_t payloadSize,
    ProtocolByteLimit messageLimit) noexcept {
    return !isWebSocketControlFrameKind(kind) &&
        webSocketFrameLengthExceedsLimit(payloadSize, messageLimit);
}

[[nodiscard]] inline bool webSocketMaskedFrameReadSizeOverflows(
    std::uint64_t payloadSize,
    std::size_t headerSize) noexcept {
    constexpr std::size_t kMaskBytes = 4;
    constexpr auto kMaxSize = (std::numeric_limits<std::size_t>::max)();
    return headerSize > kMaxSize - kMaskBytes ||
        payloadSize > static_cast<std::uint64_t>(kMaxSize - headerSize - kMaskBytes);
}

[[nodiscard]] inline std::uint16_t readWebSocketUint16(const char* data) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(static_cast<unsigned char>(data[0])) << 8) |
        static_cast<unsigned char>(data[1]));
}

[[nodiscard]] inline bool readWebSocketUint64(const char* data, std::uint64_t& value) noexcept {
    if ((static_cast<unsigned char>(data[0]) & 0x80U) != 0) {
        return false;
    }
    value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<unsigned char>(data[i]);
    }
    return true;
}

[[nodiscard]] inline std::size_t encodeWebSocketFrameHeader(
    WebSocketFrameHeader& header,
    WebSocketOpcode opcode,
    std::size_t payloadSize,
    bool rsv1 = false) noexcept {
    std::size_t headerSize = 0;
    header[headerSize++] = static_cast<char>(
        0x80U | (rsv1 ? 0x40U : 0U) | std::to_underlying(opcode));
    if (payloadSize <= 125) {
        header[headerSize++] = static_cast<char>(payloadSize);
    } else if (payloadSize <= 0xFFFF) {
        header[headerSize++] = static_cast<char>(126);
        header[headerSize++] = static_cast<char>((payloadSize >> 8) & 0xFF);
        header[headerSize++] = static_cast<char>(payloadSize & 0xFF);
    } else {
        header[headerSize++] = static_cast<char>(127);
        const auto size = static_cast<std::uint64_t>(payloadSize);
        for (int shift = 56; shift >= 0; shift -= 8) {
            header[headerSize++] = static_cast<char>((size >> shift) & 0xFF);
        }
    }
    return headerSize;
}

inline void decodeMaskedWebSocketPayload(char* payload, std::size_t payloadSize, const char* mask) noexcept {
    const auto m0 = static_cast<unsigned char>(mask[0]);
    const auto m1 = static_cast<unsigned char>(mask[1]);
    const auto m2 = static_cast<unsigned char>(mask[2]);
    const auto m3 = static_cast<unsigned char>(mask[3]);
    std::size_t i = 0;
    for (; i + 4 <= payloadSize; i += 4) {
        payload[i] = static_cast<char>(static_cast<unsigned char>(payload[i]) ^ m0);
        payload[i + 1] = static_cast<char>(static_cast<unsigned char>(payload[i + 1]) ^ m1);
        payload[i + 2] = static_cast<char>(static_cast<unsigned char>(payload[i + 2]) ^ m2);
        payload[i + 3] = static_cast<char>(static_cast<unsigned char>(payload[i + 3]) ^ m3);
    }
    if (i < payloadSize) {
        payload[i] = static_cast<char>(static_cast<unsigned char>(payload[i]) ^ m0);
        ++i;
    }
    if (i < payloadSize) {
        payload[i] = static_cast<char>(static_cast<unsigned char>(payload[i]) ^ m1);
        ++i;
    }
    if (i < payloadSize) {
        payload[i] = static_cast<char>(static_cast<unsigned char>(payload[i]) ^ m2);
    }
}

inline void compactWebSocketReadBuffer(
    std::pmr::string& buffer,
    std::size_t& offset,
    std::size_t& pendingCompactUntil) noexcept {
    if (pendingCompactUntil == 0) {
        return;
    }

    const auto consumed = pendingCompactUntil;
    pendingCompactUntil = 0;
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

void encodeWebSocketAccept(WebSocketAcceptKey& output, std::string_view key);
[[nodiscard]] bool isValidWebSocketCloseCode(std::uint16_t code) noexcept;
[[nodiscard]] bool isValidUtf8(std::string_view value) noexcept;

enum class WebSocketClosePayloadEncodeError : std::uint8_t {
    kInvalidCode,
    kInvalidReason,
    kReasonTooLarge,
};

class WebSocketClosePayloadEncodeResult;

class WebSocketEncodedClosePayload final {
public:
    [[nodiscard]] constexpr std::string_view bytes() const & noexcept {
        return std::string_view(bytes_.data(), size_);
    }
    [[nodiscard]] constexpr std::string_view bytes() const && = delete;

private:
    friend class WebSocketClosePayloadEncodeResult;
    friend WebSocketClosePayloadEncodeResult encodeWebSocketClosePayload(
        std::uint16_t,
        std::string_view) noexcept;

    WebSocketEncodedClosePayload(
        std::uint16_t code,
        std::string_view reason) noexcept;

    std::array<char, 125> bytes_{};
    std::uint8_t size_{0};
};

class WebSocketClosePayloadEncodeFailure final {
public:
    [[nodiscard]] constexpr WebSocketClosePayloadEncodeError error()
        const noexcept {
        return error_;
    }

private:
    friend class WebSocketClosePayloadEncodeResult;
    friend WebSocketClosePayloadEncodeResult encodeWebSocketClosePayload(
        std::uint16_t,
        std::string_view) noexcept;

    explicit constexpr WebSocketClosePayloadEncodeFailure(
        WebSocketClosePayloadEncodeError error) noexcept
        : error_(error) {}

    WebSocketClosePayloadEncodeError error_;
};

class WebSocketClosePayloadEncodeResult final {
public:
    [[nodiscard]] constexpr const WebSocketEncodedClosePayload* encoded()
        const & noexcept {
        return std::get_if<WebSocketEncodedClosePayload>(&value_);
    }
    [[nodiscard]] constexpr const WebSocketEncodedClosePayload* encoded()
        const && = delete;

    [[nodiscard]] constexpr const WebSocketClosePayloadEncodeFailure* failure()
        const & noexcept {
        return std::get_if<WebSocketClosePayloadEncodeFailure>(&value_);
    }
    [[nodiscard]] constexpr const WebSocketClosePayloadEncodeFailure* failure()
        const && = delete;

private:
    friend WebSocketClosePayloadEncodeResult encodeWebSocketClosePayload(
        std::uint16_t,
        std::string_view) noexcept;

    explicit WebSocketClosePayloadEncodeResult(
        WebSocketEncodedClosePayload encoded) noexcept
        : value_(encoded) {}

    explicit constexpr WebSocketClosePayloadEncodeResult(
        WebSocketClosePayloadEncodeFailure failure) noexcept
        : value_(failure) {}

    using Value = std::variant<
        WebSocketEncodedClosePayload,
        WebSocketClosePayloadEncodeFailure>;
    Value value_;
};

[[nodiscard]] WebSocketClosePayloadEncodeResult encodeWebSocketClosePayload(
    std::uint16_t code,
    std::string_view reason) noexcept;
[[nodiscard]] std::optional<WebSocketProtocolFailure>
webSocketClosePayloadFailure(std::string_view payload) noexcept;
// One borrowed frame with validated metadata combinations. Payload storage must
// outlive the view, so named factories reject owning-string rvalues. They also
// keep continuation and control frames from acquiring an impossible data opcode
// or compression bit; the wire reader additionally owns masking, length, and
// Close payload validation before publishing the same type.
class WebSocketFrameView final {
public:
    [[nodiscard]] static constexpr WebSocketFrameView text(
        std::string_view payload,
        bool final,
        bool compressed = false) noexcept {
        return WebSocketFrameView(
            WebSocketFrameKind::kText, payload, final, compressed);
    }

    template <HttpTemporaryOwningCharString String>
    static WebSocketFrameView text(
        String&&,
        bool,
        bool = false) = delete;

    [[nodiscard]] static constexpr WebSocketFrameView binary(
        std::string_view payload,
        bool final,
        bool compressed = false) noexcept {
        return WebSocketFrameView(
            WebSocketFrameKind::kBinary, payload, final, compressed);
    }

    template <HttpTemporaryOwningCharString String>
    static WebSocketFrameView binary(
        String&&,
        bool,
        bool = false) = delete;

    [[nodiscard]] static constexpr WebSocketFrameView continuation(
        std::string_view payload,
        bool final) noexcept {
        return WebSocketFrameView(
            WebSocketFrameKind::kContinuation, payload, final, false);
    }

    template <HttpTemporaryOwningCharString String>
    static WebSocketFrameView continuation(String&&, bool) = delete;

    [[nodiscard]] static std::optional<WebSocketFrameView> close(
        std::string_view payload) noexcept {
        if (payload.size() > 125 ||
            webSocketClosePayloadFailure(payload).has_value()) {
            return std::nullopt;
        }
        return WebSocketFrameView(
            WebSocketFrameKind::kClose, payload, true, false);
    }

    template <HttpTemporaryOwningCharString String>
    static std::optional<WebSocketFrameView> close(String&&) = delete;

    [[nodiscard]] static constexpr std::optional<WebSocketFrameView> ping(
        std::string_view payload) noexcept {
        if (payload.size() > 125) {
            return std::nullopt;
        }
        return WebSocketFrameView(
            WebSocketFrameKind::kPing, payload, true, false);
    }

    template <HttpTemporaryOwningCharString String>
    static std::optional<WebSocketFrameView> ping(String&&) = delete;

    [[nodiscard]] static constexpr std::optional<WebSocketFrameView> pong(
        std::string_view payload) noexcept {
        if (payload.size() > 125) {
            return std::nullopt;
        }
        return WebSocketFrameView(
            WebSocketFrameKind::kPong, payload, true, false);
    }

    template <HttpTemporaryOwningCharString String>
    static std::optional<WebSocketFrameView> pong(String&&) = delete;

    [[nodiscard]] constexpr WebSocketFrameKind kind() const noexcept {
        return kind_;
    }

    [[nodiscard]] constexpr std::string_view payload() const noexcept {
        return payload_;
    }

    [[nodiscard]] constexpr bool final() const noexcept {
        return final_;
    }

    [[nodiscard]] constexpr bool compressed() const noexcept {
        return compressed_;
    }

private:
    friend class WebSocketFrameReadResult;

    constexpr WebSocketFrameView(
        const WebSocketFrameStart& start,
        std::string_view payload) noexcept
        : WebSocketFrameView(
              start.kind(), payload, start.final(), start.compressed()) {}

    constexpr WebSocketFrameView(
        WebSocketFrameKind kind,
        std::string_view payload,
        bool final,
        bool compressed) noexcept
        : kind_(kind),
          payload_(payload),
          final_(final),
          compressed_(compressed) {}

    WebSocketFrameKind kind_;
    std::string_view payload_;
    bool final_;
    bool compressed_;
};

class WebSocketInboundResult;

class WebSocketInboundContinue final {
private:
    friend class WebSocketInboundResult;
    constexpr WebSocketInboundContinue() noexcept = default;
};

class WebSocketInboundControlFrame final {
public:
    [[nodiscard]] constexpr WebSocketOpcode opcode() const noexcept {
        return opcode_;
    }

    [[nodiscard]] constexpr std::string_view payload() const noexcept {
        return payload_;
    }

private:
    friend class WebSocketInboundResult;

    constexpr WebSocketInboundControlFrame(
        WebSocketOpcode opcode,
        std::string_view payload) noexcept
        : opcode_(opcode), payload_(payload) {}

    WebSocketOpcode opcode_;
    std::string_view payload_;
};

enum class WebSocketInboundContentEncoding : std::uint8_t {
    kIdentity,
    kPerMessageDeflate,
};

class WebSocketInboundMessage final {
public:
    [[nodiscard]] constexpr const WebSocketMessage& message() const & noexcept {
        return message_;
    }
    [[nodiscard]] constexpr const WebSocketMessage& message() const && = delete;

    [[nodiscard]] constexpr WebSocketInboundContentEncoding
    contentEncoding() const noexcept {
        return contentEncoding_;
    }

private:
    friend class WebSocketInboundResult;

    constexpr WebSocketInboundMessage(
        WebSocketMessage message,
        WebSocketInboundContentEncoding contentEncoding) noexcept
        : message_(message), contentEncoding_(contentEncoding) {}

    WebSocketMessage message_;
    WebSocketInboundContentEncoding contentEncoding_;
};

class WebSocketInboundFailure final {
public:
    [[nodiscard]] constexpr WebSocketProtocolFailure error() const noexcept {
        return error_;
    }

private:
    friend class WebSocketInboundResult;

    explicit constexpr WebSocketInboundFailure(
        WebSocketProtocolFailure error) noexcept
        : error_(error) {}

    WebSocketProtocolFailure error_;
};

// A consumed frame has exactly one semantic outcome. Control payload, application
// message, and failure code live only on their corresponding alternatives; there
// is no action enum coupled to an output parameter.
class WebSocketInboundResult final {
public:
    [[nodiscard]] constexpr const WebSocketInboundContinue*
    continueReading() const & noexcept {
        return std::get_if<WebSocketInboundContinue>(&value_);
    }
    [[nodiscard]] constexpr const WebSocketInboundContinue*
    continueReading() const && = delete;

    [[nodiscard]] constexpr const WebSocketInboundControlFrame*
    controlFrame() const & noexcept {
        return std::get_if<WebSocketInboundControlFrame>(&value_);
    }
    [[nodiscard]] constexpr const WebSocketInboundControlFrame*
    controlFrame() const && = delete;

    [[nodiscard]] constexpr const WebSocketInboundMessage*
    message() const & noexcept {
        return std::get_if<WebSocketInboundMessage>(&value_);
    }
    [[nodiscard]] constexpr const WebSocketInboundMessage*
    message() const && = delete;

    [[nodiscard]] constexpr const WebSocketInboundFailure*
    failure() const & noexcept {
        return std::get_if<WebSocketInboundFailure>(&value_);
    }
    [[nodiscard]] constexpr const WebSocketInboundFailure*
    failure() const && = delete;

private:
    friend class WebSocketInboundAssembler;

    using Value = std::variant<
        WebSocketInboundContinue,
        WebSocketInboundControlFrame,
        WebSocketInboundMessage,
        WebSocketInboundFailure>;

    template <typename Alternative>
    explicit constexpr WebSocketInboundResult(
        Alternative alternative) noexcept
        : value_(alternative) {}

    [[nodiscard]] static constexpr WebSocketInboundResult
    makeContinue() noexcept {
        return WebSocketInboundResult(WebSocketInboundContinue());
    }

    [[nodiscard]] static constexpr WebSocketInboundResult
    makeControlFrame(
        WebSocketOpcode opcode,
        std::string_view payload) noexcept {
        return WebSocketInboundResult(
            WebSocketInboundControlFrame(opcode, payload));
    }

    [[nodiscard]] static constexpr WebSocketInboundResult
    makeMessage(
        WebSocketMessage message,
        WebSocketInboundContentEncoding contentEncoding) noexcept {
        return WebSocketInboundResult(
            WebSocketInboundMessage(message, contentEncoding));
    }

    [[nodiscard]] static constexpr WebSocketInboundResult
    makeFailure(WebSocketProtocolFailure error) noexcept {
        return WebSocketInboundResult(WebSocketInboundFailure(error));
    }

    Value value_;
};

// Single owner of the WebSocket inbound message-reassembly state machine (RFC
// 6455 §5.4): control-frame dispatch, fragmentation across continuation frames,
// per-message size limits and UTF-8 validation. Every wire failure is returned as
// a typed alternative carrying the exact RFC Close reason; none is thrown.
struct WebSocketInboundIdle final {};

class WebSocketInboundFragmented final {
public:
    constexpr WebSocketInboundFragmented(
        WebSocketOpcode opcode,
        WebSocketInboundContentEncoding encoding) noexcept
        : opcode_(opcode), encoding_(encoding) {}

    [[nodiscard]] constexpr WebSocketOpcode opcode() const noexcept {
        return opcode_;
    }
    [[nodiscard]] constexpr WebSocketInboundContentEncoding encoding() const noexcept {
        return encoding_;
    }

private:
    WebSocketOpcode opcode_;
    WebSocketInboundContentEncoding encoding_;
};

class WebSocketInboundAssembler final {
public:
    explicit WebSocketInboundAssembler(std::pmr::memory_resource* resource)
        : message_(resource) {}

    [[nodiscard]] WebSocketInboundResult accept(
        const WebSocketFrameView& frame,
        ProtocolByteLimit messageLimit) {
        if (isWebSocketControlFrameKind(frame.kind())) {
            const auto opcode = static_cast<WebSocketOpcode>(frame.kind());
            return WebSocketInboundResult::makeControlFrame(
                opcode, frame.payload());
        }
        if (frame.kind() == WebSocketFrameKind::kContinuation) {
            const auto* fragmented = std::get_if<WebSocketInboundFragmented>(&state_);
            if (fragmented == nullptr) {
                return WebSocketInboundResult::makeFailure(
                    WebSocketProtocolFailure::kProtocolError);
            }
            if (webSocketAppendExceedsLimit(
                    message_.size(), frame.payload().size(), messageLimit)) {
                return WebSocketInboundResult::makeFailure(
                    WebSocketProtocolFailure::kMessageTooLarge);
            }
            message_.append(frame.payload().data(), frame.payload().size());
            if (!frame.final()) {
                return WebSocketInboundResult::makeContinue();
            }
            const auto opcode = fragmented->opcode();
            const auto encoding = fragmented->encoding();
            state_.template emplace<WebSocketInboundIdle>();
            const auto message = WebSocketMessageAccess::make(
                opcode,
                std::string_view(message_.data(), message_.size()));
            if (encoding == WebSocketInboundContentEncoding::kPerMessageDeflate) {
                return WebSocketInboundResult::makeMessage(
                    message,
                    WebSocketInboundContentEncoding::kPerMessageDeflate);
            }
            if (opcode == WebSocketOpcode::kText && !isValidUtf8(message_)) {
                return WebSocketInboundResult::makeFailure(
                    WebSocketProtocolFailure::kInvalidPayloadData);
            }
            return WebSocketInboundResult::makeMessage(
                message,
                WebSocketInboundContentEncoding::kIdentity);
        }
        if (frame.kind() == WebSocketFrameKind::kText ||
            frame.kind() == WebSocketFrameKind::kBinary) {
            if (std::get_if<WebSocketInboundFragmented>(&state_) != nullptr) {
                return WebSocketInboundResult::makeFailure(
                    WebSocketProtocolFailure::kProtocolError);
            }
            if (webSocketMessageExceedsLimit(
                    frame.payload().size(), messageLimit)) {
                return WebSocketInboundResult::makeFailure(
                    WebSocketProtocolFailure::kMessageTooLarge);
            }
            const auto opcode = static_cast<WebSocketOpcode>(frame.kind());
            if (frame.final()) {
                const auto message = WebSocketMessageAccess::make(
                    opcode, frame.payload());
                if (frame.compressed()) {
                    return WebSocketInboundResult::makeMessage(
                        message,
                        WebSocketInboundContentEncoding::kPerMessageDeflate);
                }
                if (opcode == WebSocketOpcode::kText &&
                    !isValidUtf8(frame.payload())) {
                    return WebSocketInboundResult::makeFailure(
                        WebSocketProtocolFailure::kInvalidPayloadData);
                }
                return WebSocketInboundResult::makeMessage(
                    message,
                    WebSocketInboundContentEncoding::kIdentity);
            }
            state_.template emplace<WebSocketInboundFragmented>(
                opcode,
                frame.compressed()
                    ? WebSocketInboundContentEncoding::kPerMessageDeflate
                    : WebSocketInboundContentEncoding::kIdentity);
            message_.assign(frame.payload().data(), frame.payload().size());
        }
        return WebSocketInboundResult::makeContinue();
    }

private:
    std::pmr::string message_;
    std::variant<WebSocketInboundIdle, WebSocketInboundFragmented> state_;
};

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

}  // namespace detail
}  // namespace ruvia
