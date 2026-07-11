#include "test_harness.h"

#include <chrono>
#include <cstdint>

#include "ruvia/web/detail/websocket/HttpWebSocketLiveness.h"

namespace {

using ruvia::WebSocketLifecycleOptions;
using ruvia::detail::WebSocketLivenessDecision;
using ruvia::detail::WsClosePhase;
using ruvia::detail::webSocketLivenessDecision;

WebSocketLifecycleOptions options(int pingMs, int pongMs, int closeMs = 5000) {
    WebSocketLifecycleOptions opts;
    opts.pingInterval = std::chrono::milliseconds(pingMs);
    opts.pongTimeout = std::chrono::milliseconds(pongMs);
    opts.closeHandshakeTimeout = std::chrono::milliseconds(closeMs);
    return opts;
}

WebSocketLivenessDecision decide(
    const WebSocketLifecycleOptions& opts,
    WsClosePhase closePhase,
    bool awaitingPong,
    bool writeActive,
    std::int64_t lastActiveMs,
    std::int64_t pingSentMs,
    std::int64_t closeStartedMs,
    std::int64_t now) {
    return webSocketLivenessDecision(
        opts,
        closePhase,
        awaitingPong,
        writeActive,
        lastActiveMs,
        pingSentMs,
        closeStartedMs,
        now);
}

}  // namespace

RUVIA_TEST(ws_heartbeat_disabled_stays_idle) {
    // A non-positive ping interval disables the heartbeat.
    RUVIA_CHECK(decide(
        options(0, 0), WsClosePhase::kOpen, false, false, 0, 0, -1, 10000) ==
        WebSocketLivenessDecision::kIdle);
    RUVIA_CHECK(decide(
        options(1000, 500), WsClosePhase::kClosed, false, false, 0, 0, -1, 10000) ==
        WebSocketLivenessDecision::kIdle);
}

RUVIA_TEST(ws_heartbeat_sends_ping_when_idle) {
    // Not awaiting a pong, idle for >= the ping interval, and no write in flight.
    RUVIA_CHECK(decide(
        options(1000, 500), WsClosePhase::kOpen, false, false, 0, 0, -1, 2000) ==
        WebSocketLivenessDecision::kSendPing);
    // Recent activity keeps it idle.
    RUVIA_CHECK(decide(
        options(1000, 500), WsClosePhase::kOpen, false, false, 1500, 0, -1, 2000) ==
        WebSocketLivenessDecision::kIdle);
    // A write in flight defers the ping.
    RUVIA_CHECK(decide(
        options(1000, 500), WsClosePhase::kOpen, false, true, 0, 0, -1, 2000) ==
        WebSocketLivenessDecision::kIdle);
}

RUVIA_TEST(ws_heartbeat_pong_timeout) {
    // Awaiting a pong past the pong timeout -> timeout.
    RUVIA_CHECK(decide(
        options(1000, 500), WsClosePhase::kOpen, true, false, 0, 1000, -1, 1600) ==
        WebSocketLivenessDecision::kAbortTransport);
    // Still within the pong timeout -> idle.
    RUVIA_CHECK(decide(
        options(1000, 500), WsClosePhase::kOpen, true, false, 0, 1000, -1, 1400) ==
        WebSocketLivenessDecision::kIdle);
    // A non-positive pong timeout falls back to the ping interval.
    RUVIA_CHECK(decide(
        options(1000, 0), WsClosePhase::kOpen, true, false, 0, 1000, -1, 2200) ==
        WebSocketLivenessDecision::kAbortTransport);
    RUVIA_CHECK(decide(
        options(1000, 0), WsClosePhase::kOpen, true, false, 0, 1000, -1, 1500) ==
        WebSocketLivenessDecision::kIdle);
}

RUVIA_TEST(ws_liveness_bounds_local_close_handshake) {
    const auto opts = options(1000, 500, 2000);
    RUVIA_CHECK(decide(
        opts, WsClosePhase::kAwaitingPeerClose, false, false, 0, 0, 1000, 2999) ==
        WebSocketLivenessDecision::kIdle);
    RUVIA_CHECK(decide(
        opts, WsClosePhase::kAwaitingPeerClose, false, false, 0, 0, 1000, 3000) ==
        WebSocketLivenessDecision::kAbortTransport);
    RUVIA_CHECK(decide(
        options(1000, 500, 0), WsClosePhase::kAwaitingPeerClose,
        false, false, 0, 0, 1000, 100000) == WebSocketLivenessDecision::kIdle);
}
