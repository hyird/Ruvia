#include "test_harness.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>

#include "ruvia/web/detail/websocket/HttpWebSocketLiveness.h"

namespace {

using ruvia::WebSocketLifecycleOptions;
using ruvia::detail::WebSocketLivenessDecision;
using ruvia::detail::WsLivenessMode;
using ruvia::detail::webSocketLivenessDecision;

WebSocketLifecycleOptions options(int pingMs, int pongMs, int closeMs = 5000) {
    WebSocketLifecycleOptions opts;
    if (pingMs > 0) {
        opts.heartbeat = pongMs > 0
            ? ruvia::WebSocketHeartbeatPolicy::periodic(
                  std::chrono::milliseconds(pingMs),
                  std::chrono::milliseconds(pongMs))
            : ruvia::WebSocketHeartbeatPolicy::periodic(
                  std::chrono::milliseconds(pingMs));
    }
    opts.closeHandshakeTimeout = closeMs > 0
        ? std::optional<std::chrono::milliseconds>(
              std::chrono::milliseconds(closeMs))
        : std::nullopt;
    return opts;
}

WebSocketLivenessDecision decide(
    const WebSocketLifecycleOptions& opts,
    WsLivenessMode livenessMode,
    bool awaitingPong,
    bool writeActive,
    std::int64_t lastActiveMs,
    std::int64_t pingSentMs,
    std::int64_t closeStartedMs,
    std::int64_t now) {
    return webSocketLivenessDecision(
        opts,
        livenessMode,
        awaitingPong,
        writeActive,
        lastActiveMs,
        pingSentMs,
        closeStartedMs,
        now);
}

}  // namespace

RUVIA_TEST(ws_heartbeat_policy_requires_positive_durations) {
    bool zeroRejected = false;
    try {
        (void)ruvia::WebSocketHeartbeatPolicy::periodic(
            std::chrono::milliseconds(0));
    } catch (const std::invalid_argument&) {
        zeroRejected = true;
    }
    RUVIA_CHECK(zeroRejected);

    bool negativeRejected = false;
    try {
        (void)ruvia::WebSocketHeartbeatPolicy::periodic(
            std::chrono::milliseconds(1000),
            std::chrono::milliseconds(-1));
    } catch (const std::invalid_argument&) {
        negativeRejected = true;
    }
    RUVIA_CHECK(negativeRejected);
}

RUVIA_TEST(ws_heartbeat_disabled_stays_idle) {
    // Absence disables the heartbeat.
    RUVIA_CHECK(decide(
        options(0, 0), WsLivenessMode::kOpen, false, false, 0, 0, -1, 10000) ==
        WebSocketLivenessDecision::kIdle);
    RUVIA_CHECK(decide(
        options(1000, 500), WsLivenessMode::kInactive, false, false, 0, 0, -1, 10000) ==
        WebSocketLivenessDecision::kIdle);
}

RUVIA_TEST(ws_heartbeat_sends_ping_when_idle) {
    // Not awaiting a pong, idle for >= the ping interval, and no write in flight.
    RUVIA_CHECK(decide(
        options(1000, 500), WsLivenessMode::kOpen, false, false, 0, 0, -1, 2000) ==
        WebSocketLivenessDecision::kSendPing);
    // Recent activity keeps it idle.
    RUVIA_CHECK(decide(
        options(1000, 500), WsLivenessMode::kOpen, false, false, 1500, 0, -1, 2000) ==
        WebSocketLivenessDecision::kIdle);
    // A write in flight defers the ping.
    RUVIA_CHECK(decide(
        options(1000, 500), WsLivenessMode::kOpen, false, true, 0, 0, -1, 2000) ==
        WebSocketLivenessDecision::kIdle);
}

RUVIA_TEST(ws_heartbeat_pong_timeout) {
    // Awaiting a pong past the pong timeout -> timeout.
    RUVIA_CHECK(decide(
        options(1000, 500), WsLivenessMode::kOpen, true, false, 0, 1000, -1, 1600) ==
        WebSocketLivenessDecision::kAbortTransport);
    // Still within the pong timeout -> idle.
    RUVIA_CHECK(decide(
        options(1000, 500), WsLivenessMode::kOpen, true, false, 0, 1000, -1, 1400) ==
        WebSocketLivenessDecision::kIdle);
    // The one-argument policy uses the ping interval as the pong timeout.
    RUVIA_CHECK(decide(
        options(1000, 0), WsLivenessMode::kOpen, true, false, 0, 1000, -1, 2200) ==
        WebSocketLivenessDecision::kAbortTransport);
    RUVIA_CHECK(decide(
        options(1000, 0), WsLivenessMode::kOpen, true, false, 0, 1000, -1, 1500) ==
        WebSocketLivenessDecision::kIdle);
}

RUVIA_TEST(ws_liveness_bounds_local_close_handshake) {
    const auto opts = options(1000, 500, 2000);
    RUVIA_CHECK(decide(
        opts, WsLivenessMode::kAwaitingPeerClose, false, false, 0, 0, 1000, 2999) ==
        WebSocketLivenessDecision::kIdle);
    RUVIA_CHECK(decide(
        opts, WsLivenessMode::kAwaitingPeerClose, false, false, 0, 0, 1000, 3000) ==
        WebSocketLivenessDecision::kAbortTransport);
    RUVIA_CHECK(decide(
        options(1000, 500, 0), WsLivenessMode::kAwaitingPeerClose,
        false, false, 0, 0, 1000, 100000) == WebSocketLivenessDecision::kIdle);
}
