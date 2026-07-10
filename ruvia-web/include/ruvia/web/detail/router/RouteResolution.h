#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "ruvia/http/HttpTypes.h"
#include "ruvia/http/RouteModes.h"
#include "ruvia/http/WebSocketProtocol.h"

// Lightweight route-resolution result types.

namespace ruvia::detail {

class RouteEntry;

class RouteMatch final {
public:
    [[nodiscard]] std::span<const std::string_view> values() const noexcept {
        return std::span<const std::string_view>(paramValues_.data(), paramCount_);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return paramCount_;
    }

    void clear() noexcept {
        paramCount_ = 0;
    }

    void truncate(std::size_t count) noexcept {
        paramCount_ = count <= paramCount_ ? count : paramCount_;
    }

    [[nodiscard]] bool add(std::string_view value) noexcept {
        if (paramCount_ >= paramValues_.size()) {
            return false;
        }

        paramValues_[paramCount_++] = value;
        return true;
    }

private:
    std::array<std::string_view, kMaxRouteParams> paramValues_{};
    std::size_t paramCount_{0};
};

// Context-agnostic copy of the RouteEntry metadata the transport sessions read,
// so h1/h2/ws can consume it off RouteResolution without dereferencing RouteEntry
// (which lives in ruvia-web). Populated by RouteTable::resolve at match time.
struct RouteDisposition final {
    RequestBodyMode bodyMode{RequestBodyMode::kBuffered};
    ResponseBodyMode responseMode{ResponseBodyMode::kBuffered};
    std::string_view webSocketSubprotocols{};
    WebSocketHeartbeatOptions webSocketHeartbeat{};
};

struct RouteResolution final {
    [[nodiscard]] static RouteResolution foundStatic(
        const RouteEntry* route, const RouteDisposition& disposition = {}) noexcept {
        RouteResolution resolution;
        resolution.route_ = route;
        resolution.disposition_ = disposition;
        return resolution;
    }

    [[nodiscard]] static RouteResolution foundDynamic(
        const RouteEntry* route,
        const RouteMatch& match,
        const RouteDisposition& disposition = {}) noexcept {
        RouteResolution resolution;
        resolution.route_ = route;
        resolution.match_ = match;
        resolution.hasMatch_ = true;
        resolution.disposition_ = disposition;
        return resolution;
    }

    [[nodiscard]] static RouteResolution methodNotAllowed(std::uint32_t allowedMethods) noexcept {
        RouteResolution resolution;
        resolution.allowedMethods_ = allowedMethods;
        return resolution;
    }

    [[nodiscard]] bool found() const noexcept {
        return route_ != nullptr;
    }

    [[nodiscard]] bool methodNotAllowed() const noexcept {
        return route_ == nullptr && allowedMethods_ != 0;
    }

    [[nodiscard]] const RouteEntry& route() const noexcept {
        return *route_;
    }

    [[nodiscard]] const RouteMatch* match() const noexcept {
        return hasMatch_ ? &match_ : nullptr;
    }

    [[nodiscard]] std::uint32_t allowedMethods() const noexcept {
        return allowedMethods_;
    }

    // Route metadata, readable without dereferencing RouteEntry (web). Mirrors the
    // corresponding RouteEntry accessors exactly.
    [[nodiscard]] RequestBodyMode bodyMode() const noexcept {
        return disposition_.bodyMode;
    }
    [[nodiscard]] ResponseBodyMode responseMode() const noexcept {
        return disposition_.responseMode;
    }
    [[nodiscard]] bool usesStreamRequestBody() const noexcept {
        return disposition_.bodyMode == RequestBodyMode::kStream;
    }
    [[nodiscard]] bool isBufferedResponse() const noexcept {
        return disposition_.responseMode == ResponseBodyMode::kBuffered;
    }
    [[nodiscard]] bool isWebSocketResponse() const noexcept {
        return disposition_.responseMode == ResponseBodyMode::kWebSocket;
    }
    [[nodiscard]] bool usesResponseStream() const noexcept {
        return disposition_.responseMode == ResponseBodyMode::kStream ||
            disposition_.responseMode == ResponseBodyMode::kSse;
    }
    [[nodiscard]] std::string_view webSocketSubprotocols() const noexcept {
        return disposition_.webSocketSubprotocols;
    }
    [[nodiscard]] const WebSocketHeartbeatOptions& webSocketHeartbeat() const noexcept {
        return disposition_.webSocketHeartbeat;
    }

private:
    const RouteEntry* route_{nullptr};
    RouteMatch match_{};
    bool hasMatch_{false};
    std::uint32_t allowedMethods_{0};
    RouteDisposition disposition_{};
};

}  // namespace ruvia::detail
