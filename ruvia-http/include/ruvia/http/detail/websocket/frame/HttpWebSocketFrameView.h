#pragma once

#include <optional>
#include <string_view>

#include "ruvia/http/detail/util/BorrowedView.h"
#include "ruvia/http/detail/websocket/frame/HttpWebSocketFrameCodec.h"
#include "ruvia/http/detail/websocket/frame/HttpWebSocketPayloadValidation.h"

namespace ruvia::detail {

class WebSocketFrameReadResult;

// One borrowed frame with validated metadata combinations. Payload storage must
// outlive the view, so named factories reject owning-string rvalues. They also
// keep continuation and control frames from acquiring an impossible data opcode
// or compression bit; the wire reader additionally owns masking, length, and
// Close payload validation before publishing the same type.
class WebSocketFrameView final {
public:
    [[nodiscard]] static constexpr WebSocketFrameView text(
        std::string_view payload, bool final, bool compressed = false) noexcept {
        return WebSocketFrameView(WebSocketFrameKind::kText, payload, final, compressed);
    }

    template <HttpTemporaryOwningCharString String>
    static WebSocketFrameView text(String&&, bool, bool = false) = delete;

    [[nodiscard]] static constexpr WebSocketFrameView binary(
        std::string_view payload, bool final, bool compressed = false) noexcept {
        return WebSocketFrameView(WebSocketFrameKind::kBinary, payload, final, compressed);
    }

    template <HttpTemporaryOwningCharString String>
    static WebSocketFrameView binary(String&&, bool, bool = false) = delete;

    [[nodiscard]] static constexpr WebSocketFrameView continuation(
        std::string_view payload, bool final) noexcept {
        return WebSocketFrameView(WebSocketFrameKind::kContinuation, payload, final, false);
    }

    template <HttpTemporaryOwningCharString String>
    static WebSocketFrameView continuation(String&&, bool) = delete;

    [[nodiscard]] static std::optional<WebSocketFrameView> close(
        std::string_view payload) noexcept {
        if (payload.size() > 125 || webSocketClosePayloadFailure(payload).has_value()) {
            return std::nullopt;
        }
        return WebSocketFrameView(WebSocketFrameKind::kClose, payload, true, false);
    }

    template <HttpTemporaryOwningCharString String>
    static std::optional<WebSocketFrameView> close(String&&) = delete;

    [[nodiscard]] static constexpr std::optional<WebSocketFrameView> ping(
        std::string_view payload) noexcept {
        if (payload.size() > 125) {
            return std::nullopt;
        }
        return WebSocketFrameView(WebSocketFrameKind::kPing, payload, true, false);
    }

    template <HttpTemporaryOwningCharString String>
    static std::optional<WebSocketFrameView> ping(String&&) = delete;

    [[nodiscard]] static constexpr std::optional<WebSocketFrameView> pong(
        std::string_view payload) noexcept {
        if (payload.size() > 125) {
            return std::nullopt;
        }
        return WebSocketFrameView(WebSocketFrameKind::kPong, payload, true, false);
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
        const WebSocketFrameStart& start, std::string_view payload) noexcept
        : WebSocketFrameView(start.kind(), payload, start.final(), start.compressed()) {}

    constexpr WebSocketFrameView(
        WebSocketFrameKind kind, std::string_view payload, bool final, bool compressed) noexcept
        : kind_(kind),
          payload_(payload),
          final_(final),
          compressed_(compressed) {}

    WebSocketFrameKind kind_;
    std::string_view payload_;
    bool final_;
    bool compressed_;
};
}  // namespace ruvia::detail
