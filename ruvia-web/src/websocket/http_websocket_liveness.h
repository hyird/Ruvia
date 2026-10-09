#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <variant>

#include "ruvia/http/websocket_protocol.h"
#include "ruvia/web/websocket.h"

namespace ruvia::detail {

[[nodiscard]] inline std::int64_t websocket_steady_now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

enum class websocket_liveness_decision : std::uint8_t {
    idle,
    send_ping,
    abort_transport,
};

class websocket_liveness_idle final {};

// The ping write and its Pong wait are separate states. A Pong may arrive as
// soon as the transport accepts the bytes and before the write coroutine is
// resumed, so the in-flight state must be visible to the reader without
// starting the timeout prematurely.
class websocket_sending_ping final {
public:
    explicit websocket_sending_ping(std::uint64_t challenge) noexcept
        : challenge_(challenge) {}

    [[nodiscard]] std::uint64_t challenge() const noexcept {
        return challenge_;
    }

private:
    std::uint64_t challenge_;
};

class websocket_awaiting_pong final {
public:
    explicit websocket_awaiting_pong(std::int64_t sent_at_ms, std::uint64_t challenge) noexcept
        : sent_at_ms_(sent_at_ms),
          challenge_(challenge) {}

    [[nodiscard]] std::int64_t sent_at_ms() const noexcept {
        return sent_at_ms_;
    }

    [[nodiscard]] std::uint64_t challenge() const noexcept {
        return challenge_;
    }

private:
    std::int64_t sent_at_ms_;
    std::uint64_t challenge_;
};

class websocket_awaiting_peer_close final {
public:
    explicit websocket_awaiting_peer_close(std::int64_t started_at_ms) noexcept
        : started_at_ms_(started_at_ms) {}

    [[nodiscard]] std::int64_t started_at_ms() const noexcept {
        return started_at_ms_;
    }

private:
    std::int64_t started_at_ms_;
};

using websocket_liveness_state_type = std::variant<websocket_liveness_idle, websocket_sending_ping,
    websocket_awaiting_pong, websocket_awaiting_peer_close>;

[[nodiscard]] inline std::array<char, 8> websocket_heartbeat_payload(std::uint64_t challenge) noexcept {
    std::array<char, 8> payload_value{};
    for (std::size_t i = 0; i < payload_value.size(); ++i) {
        payload_value[i] = static_cast<char>(challenge >> ((payload_value.size() - 1 - i) * 8));
    }
    return payload_value;
}

[[nodiscard]] inline bool websocket_heartbeat_pong_matches(
    const websocket_liveness_state_type& state_value, std::string_view payload_value) noexcept {
    std::uint64_t challenge;
    if (const auto* sending = std::get_if<websocket_sending_ping>(&state_value)) {
        challenge = sending->challenge();
    } else if (const auto* awaiting = std::get_if<websocket_awaiting_pong>(&state_value)) {
        challenge = awaiting->challenge();
    } else {
        return false;
    }
    const auto expected = websocket_heartbeat_payload(challenge);
    return payload_value == std::string_view(expected.data(), expected.size());
}

[[nodiscard]] inline websocket_liveness_decision evaluate_websocket_liveness(
    const websocket_lifecycle_options& options, websocket_liveness_mode liveness_mode,
    const websocket_liveness_state_type& state_value, bool write_active, std::int64_t last_active_ms,
    std::int64_t now) noexcept {
    if (liveness_mode != websocket_liveness_mode::open) {
        const auto* close = std::get_if<websocket_awaiting_peer_close>(&state_value);
        return liveness_mode == websocket_liveness_mode::awaiting_peer_close && close != nullptr &&
                       options.close_handshake_timeout_.has_value() &&
                       now - close->started_at_ms() >= options.close_handshake_timeout_->count()
                   ? websocket_liveness_decision::abort_transport
                   : websocket_liveness_decision::idle;
    }

    if (!options.heartbeat_.ping_interval_.has_value()) {
        return websocket_liveness_decision::idle;
    }

    const auto ping_interval = options.heartbeat_.ping_interval_->count();
    const auto pong_timeout = options.heartbeat_.pong_timeout_->count();
    if (const auto* pong = std::get_if<websocket_awaiting_pong>(&state_value)) {
        return now - pong->sent_at_ms() >= pong_timeout ? websocket_liveness_decision::abort_transport
                                                        : websocket_liveness_decision::idle;
    }
    if (!std::holds_alternative<websocket_liveness_idle>(state_value)) {
        return websocket_liveness_decision::idle;
    }
    if (now - last_active_ms < ping_interval || write_active) {
        return websocket_liveness_decision::idle;
    }
    return websocket_liveness_decision::send_ping;
}

}  // namespace ruvia::detail
