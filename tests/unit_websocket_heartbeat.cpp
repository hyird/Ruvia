#include "test_harness.h"

#include <chrono>
#include <cstdint>

#include "net/ws/HttpWebSocketUtils.h"
#include "ruvia/http/WebSocketProtocol.h"

namespace {

using ruvia::WebSocketHeartbeatOptions;
using ruvia::detail::WebSocketHeartbeatDecision;
using ruvia::detail::webSocketHeartbeatDecision;

WebSocketHeartbeatOptions options(int pingMs, int pongMs) {
    WebSocketHeartbeatOptions opts;
    opts.pingInterval = std::chrono::milliseconds(pingMs);
    opts.pongTimeout = std::chrono::milliseconds(pongMs);
    return opts;
}

WebSocketHeartbeatDecision decide(
    const WebSocketHeartbeatOptions& opts, bool closeSent, bool awaitingPong,
    bool writeActive, std::int64_t lastActiveMs, std::int64_t pingSentMs, std::int64_t now) {
    return webSocketHeartbeatDecision(opts, closeSent, awaitingPong, writeActive, lastActiveMs, pingSentMs, now);
}

}  // namespace

RUVIA_TEST(ws_heartbeat_disabled_stays_idle) {
    // A non-positive ping interval disables the heartbeat.
    RUVIA_CHECK(decide(options(0, 0), false, false, false, 0, 0, 10000) ==
                WebSocketHeartbeatDecision::kIdle);
    // Once a Close has been sent the heartbeat is idle regardless.
    RUVIA_CHECK(decide(options(1000, 500), true, false, false, 0, 0, 10000) ==
                WebSocketHeartbeatDecision::kIdle);
}

RUVIA_TEST(ws_heartbeat_sends_ping_when_idle) {
    // Not awaiting a pong, idle for >= the ping interval, and no write in flight.
    RUVIA_CHECK(decide(options(1000, 500), false, false, false, 0, 0, 2000) ==
                WebSocketHeartbeatDecision::kSendPing);
    // Recent activity keeps it idle.
    RUVIA_CHECK(decide(options(1000, 500), false, false, false, 1500, 0, 2000) ==
                WebSocketHeartbeatDecision::kIdle);
    // A write in flight defers the ping.
    RUVIA_CHECK(decide(options(1000, 500), false, false, true, 0, 0, 2000) ==
                WebSocketHeartbeatDecision::kIdle);
}

RUVIA_TEST(ws_heartbeat_pong_timeout) {
    // Awaiting a pong past the pong timeout -> timeout.
    RUVIA_CHECK(decide(options(1000, 500), false, true, false, 0, 1000, 1600) ==
                WebSocketHeartbeatDecision::kTimeout);
    // Still within the pong timeout -> idle.
    RUVIA_CHECK(decide(options(1000, 500), false, true, false, 0, 1000, 1400) ==
                WebSocketHeartbeatDecision::kIdle);
    // A non-positive pong timeout falls back to the ping interval.
    RUVIA_CHECK(decide(options(1000, 0), false, true, false, 0, 1000, 2200) ==
                WebSocketHeartbeatDecision::kTimeout);
    RUVIA_CHECK(decide(options(1000, 0), false, true, false, 0, 1000, 1500) ==
                WebSocketHeartbeatDecision::kIdle);
}
