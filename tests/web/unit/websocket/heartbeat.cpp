#include "test_harness.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>

#include "ruvia/web/detail/websocket/HttpWebSocketLiveness.h"

namespace {

using ruvia::WebSocketLifecycleOptions;
using ruvia::detail::WebSocketAwaitingPeerClose;
using ruvia::detail::WebSocketAwaitingPong;
using ruvia::detail::WebSocketLivenessDecision;
using ruvia::detail::webSocketLivenessDecision;
using ruvia::detail::WebSocketLivenessIdle;
using ruvia::detail::WebSocketLivenessState;
using ruvia::detail::WebSocketSendingPing;
using ruvia::detail::WsLivenessMode;

WebSocketLifecycleOptions options(int pingMs, int pongMs, int closeMs = 5000) {
    WebSocketLifecycleOptions opts;
    if (pingMs > 0) {
        opts.heartbeat = ruvia::WebSocketHeartbeatPolicy::periodic({
            .pingInterval = std::chrono::milliseconds(pingMs),
            .pongTimeout = pongMs > 0
                ? std::optional<std::chrono::milliseconds>(std::chrono::milliseconds(pongMs))
                : std::nullopt,
        });
    }
    opts.closeHandshakeTimeout = closeMs > 0 ? std::optional<std::chrono::milliseconds>(std::chrono::milliseconds(closeMs)) : std::nullopt;
    return opts;
}

WebSocketLivenessDecision decide(const WebSocketLifecycleOptions& opts, WsLivenessMode livenessMode, WebSocketLivenessState state, bool writeActive, std::int64_t lastActiveMs, std::int64_t now) {
    return webSocketLivenessDecision(opts, livenessMode, state, writeActive, lastActiveMs, now);
}

}  // namespace

RUVIA_TEST(ws_heartbeat_policy_requires_positive_durations) {
    bool zeroRejected = false;
    try {
        (void)ruvia::WebSocketHeartbeatPolicy::periodic({
            .pingInterval = std::chrono::milliseconds(0),
        });
    } catch (const std::invalid_argument&) {
        zeroRejected = true;
    }
    RUVIA_CHECK(zeroRejected);

    bool negativeRejected = false;
    try {
        (void)ruvia::WebSocketHeartbeatPolicy::periodic({
            .pingInterval = std::chrono::milliseconds(1000),
            .pongTimeout = std::chrono::milliseconds(-1),
        });
    } catch (const std::invalid_argument&) {
        negativeRejected = true;
    }
    RUVIA_CHECK(negativeRejected);
}

RUVIA_TEST(ws_heartbeat_disabled_stays_idle) {
    // Absence disables the heartbeat.
    RUVIA_CHECK(decide(options(0, 0), WsLivenessMode::kOpen, WebSocketLivenessIdle{}, false, 0, 10000) == WebSocketLivenessDecision::kIdle);
    RUVIA_CHECK(decide(options(1000, 500), WsLivenessMode::kInactive, WebSocketLivenessIdle{}, false, 0, 10000) == WebSocketLivenessDecision::kIdle);
}

RUVIA_TEST(ws_heartbeat_sends_ping_when_idle) {
    // Not awaiting a pong, idle for >= the ping interval, and no write in flight.
    RUVIA_CHECK(decide(options(1000, 500), WsLivenessMode::kOpen, WebSocketLivenessIdle{}, false, 0, 2000) == WebSocketLivenessDecision::kSendPing);
    // Recent activity keeps it idle.
    RUVIA_CHECK(decide(options(1000, 500), WsLivenessMode::kOpen, WebSocketLivenessIdle{}, false, 1500, 2000) == WebSocketLivenessDecision::kIdle);
    // A write in flight defers the ping.
    RUVIA_CHECK(decide(options(1000, 500), WsLivenessMode::kOpen, WebSocketLivenessIdle{}, true, 0, 2000) == WebSocketLivenessDecision::kIdle);
}

RUVIA_TEST(ws_heartbeat_pong_timeout) {
    // Awaiting a pong past the pong timeout -> timeout.
    RUVIA_CHECK(decide(options(1000, 500), WsLivenessMode::kOpen, WebSocketAwaitingPong(1000), false, 0, 1600) == WebSocketLivenessDecision::kAbortTransport);
    // Still within the pong timeout -> idle.
    RUVIA_CHECK(decide(options(1000, 500), WsLivenessMode::kOpen, WebSocketAwaitingPong(1000), false, 0, 1400) == WebSocketLivenessDecision::kIdle);
    // Omitting pongTimeout uses the ping interval as the pong timeout.
    RUVIA_CHECK(decide(options(1000, 0), WsLivenessMode::kOpen, WebSocketAwaitingPong(1000), false, 0, 2200) == WebSocketLivenessDecision::kAbortTransport);
    RUVIA_CHECK(decide(options(1000, 0), WsLivenessMode::kOpen, WebSocketAwaitingPong(1000), false, 0, 1500) == WebSocketLivenessDecision::kIdle);
}

RUVIA_TEST(ws_liveness_bounds_local_close_handshake) {
    const auto opts = options(1000, 500, 2000);
    RUVIA_CHECK(decide(opts, WsLivenessMode::kAwaitingPeerClose, WebSocketAwaitingPeerClose(1000), false, 0, 2999) == WebSocketLivenessDecision::kIdle);
    RUVIA_CHECK(decide(opts, WsLivenessMode::kAwaitingPeerClose, WebSocketAwaitingPeerClose(1000), false, 0, 3000) == WebSocketLivenessDecision::kAbortTransport);
    RUVIA_CHECK(decide(options(1000, 500, 0), WsLivenessMode::kAwaitingPeerClose, WebSocketAwaitingPeerClose(1000), false, 0, 100000) == WebSocketLivenessDecision::kIdle);
}

RUVIA_TEST(ws_liveness_state_makes_pong_and_close_waits_exclusive) {
    const WebSocketLivenessState sending = WebSocketSendingPing{};
    const WebSocketLivenessState pong = WebSocketAwaitingPong(1000);
    const WebSocketLivenessState close = WebSocketAwaitingPeerClose(2000);
    RUVIA_CHECK(std::holds_alternative<WebSocketSendingPing>(sending));
    RUVIA_CHECK(!std::holds_alternative<WebSocketAwaitingPong>(sending));
    RUVIA_CHECK(std::holds_alternative<WebSocketAwaitingPong>(pong));
    RUVIA_CHECK(!std::holds_alternative<WebSocketAwaitingPeerClose>(pong));
    RUVIA_CHECK(std::holds_alternative<WebSocketAwaitingPeerClose>(close));
    RUVIA_CHECK(!std::holds_alternative<WebSocketAwaitingPong>(close));
}
