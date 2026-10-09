#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>

#include "test_harness.h"
#include "websocket/http_websocket_liveness.h"

namespace {

using ruvia::websocket_lifecycle_options;
using ruvia::websocket_liveness_mode;
using ruvia::detail::websocket_awaiting_peer_close;
using ruvia::detail::websocket_awaiting_pong;
using ruvia::detail::websocket_liveness_decision;
using ruvia::detail::websocket_liveness_idle;
using ruvia::detail::websocket_liveness_state_type;
using ruvia::detail::websocket_sending_ping;

websocket_lifecycle_options options(int ping_ms, int pong_ms, int close_ms = 5000) {
    websocket_lifecycle_options opts;
    if (ping_ms > 0) {
        opts.heartbeat_.ping_interval_ = std::chrono::milliseconds(ping_ms);
        opts.heartbeat_.pong_timeout_ = std::chrono::milliseconds(pong_ms > 0 ? pong_ms : ping_ms);
    }
    opts.close_handshake_timeout_ =
        close_ms > 0 ? std::optional<std::chrono::milliseconds>(std::chrono::milliseconds(close_ms))
                     : std::nullopt;
    return opts;
}

websocket_liveness_decision decide(const websocket_lifecycle_options& opts, websocket_liveness_mode liveness_mode,
    websocket_liveness_state_type state_value, bool write_active, std::int64_t last_active_ms, std::int64_t now) {
    return ruvia::detail::evaluate_websocket_liveness(opts, liveness_mode, state_value, write_active, last_active_ms, now);
}

}  // namespace

RUVIA_TEST(ws_heartbeat_config_is_a_plain_optional_value) {
    const ruvia::websocket_heartbeat_config disabled;
    RUVIA_CHECK(!disabled.ping_interval_.has_value());
}

RUVIA_TEST(ws_heartbeat_disabled_stays_idle) {
    // Absence disables the heartbeat.
    RUVIA_CHECK(decide(options(0, 0), websocket_liveness_mode::open, websocket_liveness_idle{}, false, 0,
                    10000) == websocket_liveness_decision::idle);
    RUVIA_CHECK(decide(options(1000, 500), websocket_liveness_mode::inactive, websocket_liveness_idle{},
                    false, 0, 10000) == websocket_liveness_decision::idle);
}

RUVIA_TEST(ws_heartbeat_sends_ping_when_idle) {
    // Not awaiting a pong, idle for >= the ping interval, and no write in flight.
    RUVIA_CHECK(decide(options(1000, 500), websocket_liveness_mode::open, websocket_liveness_idle{}, false, 0,
                    2000) == websocket_liveness_decision::send_ping);
    // Recent activity keeps it idle.
    RUVIA_CHECK(decide(options(1000, 500), websocket_liveness_mode::open, websocket_liveness_idle{}, false,
                    1500, 2000) == websocket_liveness_decision::idle);
    // A write in flight defers the ping.
    RUVIA_CHECK(decide(options(1000, 500), websocket_liveness_mode::open, websocket_liveness_idle{}, true, 0,
                    2000) == websocket_liveness_decision::idle);
}

RUVIA_TEST(ws_heartbeat_pong_timeout) {
    // Awaiting a pong past the pong timeout -> timeout.
    RUVIA_CHECK(decide(options(1000, 500), websocket_liveness_mode::open, websocket_awaiting_pong(1000, 1),
                    false, 0, 1600) == websocket_liveness_decision::abort_transport);
    // Still within the pong timeout -> idle.
    RUVIA_CHECK(decide(options(1000, 500), websocket_liveness_mode::open, websocket_awaiting_pong(1000, 1),
                    false, 0, 1400) == websocket_liveness_decision::idle);
    // Omitting pong_timeout uses the ping interval as the pong timeout.
    RUVIA_CHECK(decide(options(1000, 0), websocket_liveness_mode::open, websocket_awaiting_pong(1000, 1), false,
                    0, 2200) == websocket_liveness_decision::abort_transport);
    RUVIA_CHECK(decide(options(1000, 0), websocket_liveness_mode::open, websocket_awaiting_pong(1000, 1), false,
                    0, 1500) == websocket_liveness_decision::idle);
}

RUVIA_TEST(ws_heartbeat_matches_only_the_current_ping_payload) {
    const auto first = ruvia::detail::websocket_heartbeat_payload(1);
    const auto second = ruvia::detail::websocket_heartbeat_payload(2);
    const std::string_view first_view(first.data(), first.size());
    const std::string_view second_view(second.data(), second.size());
    RUVIA_CHECK(first_view != second_view);
    RUVIA_CHECK(ruvia::detail::websocket_heartbeat_pong_matches(websocket_sending_ping(1), first_view));
    RUVIA_CHECK(ruvia::detail::websocket_heartbeat_pong_matches(websocket_awaiting_pong(1000, 1), first_view));
    RUVIA_CHECK(!ruvia::detail::websocket_heartbeat_pong_matches(websocket_awaiting_pong(1000, 2), first_view));
    RUVIA_CHECK(!ruvia::detail::websocket_heartbeat_pong_matches(websocket_awaiting_pong(1000, 1), {}));
    RUVIA_CHECK(!ruvia::detail::websocket_heartbeat_pong_matches(websocket_liveness_idle{}, second_view));
}

RUVIA_TEST(ws_liveness_bounds_local_close_handshake) {
    const auto opts = options(1000, 500, 2000);
    RUVIA_CHECK(decide(opts, websocket_liveness_mode::awaiting_peer_close, websocket_awaiting_peer_close(1000),
                    false, 0, 2999) == websocket_liveness_decision::idle);
    RUVIA_CHECK(decide(opts, websocket_liveness_mode::awaiting_peer_close, websocket_awaiting_peer_close(1000),
                    false, 0, 3000) == websocket_liveness_decision::abort_transport);
    RUVIA_CHECK(decide(options(1000, 500, 0), websocket_liveness_mode::awaiting_peer_close,
                    websocket_awaiting_peer_close(1000), false, 0,
                    100000) == websocket_liveness_decision::idle);
}

RUVIA_TEST(ws_liveness_state_makes_pong_and_close_waits_exclusive) {
    const websocket_liveness_state_type sending = websocket_sending_ping(1);
    const websocket_liveness_state_type pong = websocket_awaiting_pong(1000, 1);
    const websocket_liveness_state_type close = websocket_awaiting_peer_close(2000);
    RUVIA_CHECK(std::holds_alternative<websocket_sending_ping>(sending));
    RUVIA_CHECK(!std::holds_alternative<websocket_awaiting_pong>(sending));
    RUVIA_CHECK(std::holds_alternative<websocket_awaiting_pong>(pong));
    RUVIA_CHECK(!std::holds_alternative<websocket_awaiting_peer_close>(pong));
    RUVIA_CHECK(std::holds_alternative<websocket_awaiting_peer_close>(close));
    RUVIA_CHECK(!std::holds_alternative<websocket_awaiting_pong>(close));
}
