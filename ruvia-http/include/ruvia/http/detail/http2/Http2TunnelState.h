#pragma once

#include <cstdint>

namespace ruvia::detail {

enum class Http2ConnectKind : std::uint8_t {
    kNone,
    kStandard,
    kExtended
};

enum class Http2TunnelPhase : std::uint8_t {
    kNone,
    kAwaitingResponse,
    kOpen,
    kRejected
};

// CONNECT has no request content. Only a successful final response changes the
// following DATA into opaque tunnel bytes, so this state is deliberately separate
// from request/response Content-Length accounting.
class Http2TunnelState final {
public:
    [[nodiscard]] bool begin(Http2ConnectKind kind) noexcept {
        if (kind == Http2ConnectKind::kNone || isConnect()) {
            return false;
        }
        kind_ = kind;
        phase_ = Http2TunnelPhase::kAwaitingResponse;
        return true;
    }

    [[nodiscard]] Http2ConnectKind kind() const noexcept { return kind_; }
    [[nodiscard]] Http2TunnelPhase phase() const noexcept { return phase_; }
    [[nodiscard]] bool isConnect() const noexcept { return kind_ != Http2ConnectKind::kNone; }
    [[nodiscard]] bool standard() const noexcept { return kind_ == Http2ConnectKind::kStandard; }
    [[nodiscard]] bool extended() const noexcept { return kind_ == Http2ConnectKind::kExtended; }
    [[nodiscard]] bool awaitingResponse() const noexcept {
        return phase_ == Http2TunnelPhase::kAwaitingResponse;
    }
    [[nodiscard]] bool open() const noexcept { return phase_ == Http2TunnelPhase::kOpen; }
    [[nodiscard]] bool rejected() const noexcept { return phase_ == Http2TunnelPhase::kRejected; }

    [[nodiscard]] bool accept() noexcept {
        if (!awaitingResponse()) {
            return false;
        }
        phase_ = Http2TunnelPhase::kOpen;
        return true;
    }

    [[nodiscard]] bool reject() noexcept {
        if (!awaitingResponse()) {
            return false;
        }
        phase_ = Http2TunnelPhase::kRejected;
        return true;
    }

private:
    Http2ConnectKind kind_{Http2ConnectKind::kNone};
    Http2TunnelPhase phase_{Http2TunnelPhase::kNone};
};

}  // namespace ruvia::detail
