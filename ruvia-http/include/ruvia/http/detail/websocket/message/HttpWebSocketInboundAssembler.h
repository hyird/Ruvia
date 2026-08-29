#pragma once

#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <variant>

#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/WebSocketProtocol.h"
#include "ruvia/http/detail/websocket/frame/HttpWebSocketFrameCodec.h"
#include "ruvia/http/detail/websocket/frame/HttpWebSocketFrameView.h"
#include "ruvia/http/detail/websocket/message/HttpWebSocketMessageAccess.h"
#include "ruvia/http/detail/websocket/frame/HttpWebSocketPayloadValidation.h"

namespace ruvia::detail {

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
        WebSocketOpcode opcode, std::string_view payload) noexcept
        : opcode_(opcode),
          payload_(payload) {}

    WebSocketOpcode opcode_;
    std::string_view payload_;
};

enum class WebSocketInboundContentEncoding : std::uint8_t {
    kIdentity,
    kPerMessageDeflate,
};

class WebSocketInboundMessage final {
public:
    [[nodiscard]] constexpr const WebSocketMessage& message() const& noexcept {
        return message_;
    }
    [[nodiscard]] constexpr const WebSocketMessage& message() const&& = delete;

    [[nodiscard]] constexpr WebSocketInboundContentEncoding contentEncoding() const noexcept {
        return contentEncoding_;
    }

private:
    friend class WebSocketInboundResult;

    constexpr WebSocketInboundMessage(
        WebSocketMessage message, WebSocketInboundContentEncoding contentEncoding) noexcept
        : message_(message),
          contentEncoding_(contentEncoding) {}

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

    explicit constexpr WebSocketInboundFailure(WebSocketProtocolFailure error) noexcept
        : error_(error) {}

    WebSocketProtocolFailure error_;
};

// A consumed frame has exactly one semantic outcome. Control payload, application
// message, and failure code live only on their corresponding alternatives; there
// is no action enum coupled to an output parameter.
class WebSocketInboundResult final {
public:
    [[nodiscard]] constexpr const WebSocketInboundContinue* continueReading() const& noexcept {
        return std::get_if<WebSocketInboundContinue>(&value_);
    }
    [[nodiscard]] constexpr const WebSocketInboundContinue* continueReading() const&& = delete;

    [[nodiscard]] constexpr const WebSocketInboundControlFrame* controlFrame() const& noexcept {
        return std::get_if<WebSocketInboundControlFrame>(&value_);
    }
    [[nodiscard]] constexpr const WebSocketInboundControlFrame* controlFrame() const&& = delete;

    [[nodiscard]] constexpr const WebSocketInboundMessage* message() const& noexcept {
        return std::get_if<WebSocketInboundMessage>(&value_);
    }
    [[nodiscard]] constexpr const WebSocketInboundMessage* message() const&& = delete;

    [[nodiscard]] constexpr const WebSocketInboundFailure* failure() const& noexcept {
        return std::get_if<WebSocketInboundFailure>(&value_);
    }
    [[nodiscard]] constexpr const WebSocketInboundFailure* failure() const&& = delete;

private:
    friend class WebSocketInboundAssembler;

    using Value = std::variant<WebSocketInboundContinue, WebSocketInboundControlFrame,
        WebSocketInboundMessage, WebSocketInboundFailure>;

    template <typename Alternative>
    explicit constexpr WebSocketInboundResult(Alternative alternative) noexcept
        : value_(alternative) {}

    [[nodiscard]] static constexpr WebSocketInboundResult makeContinue() noexcept {
        return WebSocketInboundResult(WebSocketInboundContinue());
    }

    [[nodiscard]] static constexpr WebSocketInboundResult makeControlFrame(
        WebSocketOpcode opcode, std::string_view payload) noexcept {
        return WebSocketInboundResult(WebSocketInboundControlFrame(opcode, payload));
    }

    [[nodiscard]] static constexpr WebSocketInboundResult makeMessage(
        WebSocketMessage message, WebSocketInboundContentEncoding contentEncoding) noexcept {
        return WebSocketInboundResult(WebSocketInboundMessage(message, contentEncoding));
    }

    [[nodiscard]] static constexpr WebSocketInboundResult makeFailure(
        WebSocketProtocolFailure error) noexcept {
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
        WebSocketOpcode opcode, WebSocketInboundContentEncoding encoding) noexcept
        : opcode_(opcode),
          encoding_(encoding) {}

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
        const WebSocketFrameView& frame, ProtocolByteLimit messageLimit) {
        if (isWebSocketControlFrameKind(frame.kind())) {
            const auto opcode = static_cast<WebSocketOpcode>(frame.kind());
            return WebSocketInboundResult::makeControlFrame(opcode, frame.payload());
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
            const auto message = WebSocketMessageAccess::make(opcode, std::string_view(message_));
            if (encoding == WebSocketInboundContentEncoding::kPerMessageDeflate) {
                return WebSocketInboundResult::makeMessage(
                    message, WebSocketInboundContentEncoding::kPerMessageDeflate);
            }
            if (opcode == WebSocketOpcode::kText && !isValidUtf8(message_)) {
                return WebSocketInboundResult::makeFailure(
                    WebSocketProtocolFailure::kInvalidPayloadData);
            }
            return WebSocketInboundResult::makeMessage(
                message, WebSocketInboundContentEncoding::kIdentity);
        }
        if (frame.kind() == WebSocketFrameKind::kText ||
            frame.kind() == WebSocketFrameKind::kBinary) {
            if (std::get_if<WebSocketInboundFragmented>(&state_) != nullptr) {
                return WebSocketInboundResult::makeFailure(
                    WebSocketProtocolFailure::kProtocolError);
            }
            if (webSocketMessageExceedsLimit(frame.payload().size(), messageLimit)) {
                return WebSocketInboundResult::makeFailure(
                    WebSocketProtocolFailure::kMessageTooLarge);
            }
            const auto opcode = static_cast<WebSocketOpcode>(frame.kind());
            if (frame.final()) {
                const auto message = WebSocketMessageAccess::make(opcode, frame.payload());
                if (frame.compressed()) {
                    return WebSocketInboundResult::makeMessage(
                        message, WebSocketInboundContentEncoding::kPerMessageDeflate);
                }
                if (opcode == WebSocketOpcode::kText && !isValidUtf8(frame.payload())) {
                    return WebSocketInboundResult::makeFailure(
                        WebSocketProtocolFailure::kInvalidPayloadData);
                }
                return WebSocketInboundResult::makeMessage(
                    message, WebSocketInboundContentEncoding::kIdentity);
            }
            std::pmr::string staged(message_.get_allocator());
            staged.assign(frame.payload().data(), frame.payload().size());
            message_.swap(staged);
            state_.template emplace<WebSocketInboundFragmented>(
                opcode, frame.compressed() ? WebSocketInboundContentEncoding::kPerMessageDeflate
                                           : WebSocketInboundContentEncoding::kIdentity);
        }
        return WebSocketInboundResult::makeContinue();
    }

private:
    std::pmr::string message_;
    std::variant<WebSocketInboundIdle, WebSocketInboundFragmented> state_;
};
}  // namespace ruvia::detail
