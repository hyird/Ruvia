#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/websocket_protocol.h"
#include "ruvia/web/websocket.h"

#include "util/steady_clock.h"

namespace ruvia::detail {

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

// pong_observable: an application read is polling the core and no write holds
// the lane, so a Pong that arrived can actually be seen. Only then does the
// Pong wait count; a peer that answered during a long write, or while no read
// is pending, is not dead (RFC 6455 section 5.5.3 only asks the peer to answer
// promptly). A write that stops making progress is still caught below.
[[nodiscard]] inline websocket_liveness_decision evaluate_websocket_liveness(
    const websocket_lifecycle_options& options, websocket_liveness_mode liveness_mode,
    const websocket_liveness_state_type& state_value, bool write_active, bool pong_observable,
    std::int64_t last_active_ms, std::int64_t now) noexcept {
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
    const auto* pong = std::get_if<websocket_awaiting_pong>(&state_value);
    if (pong != nullptr && pong_observable) {
        return now - pong->sent_at_ms() >= pong_timeout ? websocket_liveness_decision::abort_transport
                                                        : websocket_liveness_decision::idle;
    }
    // A write that holds the lane (including a heartbeat Ping) blocks the next
    // Ping, so a peer that neither reads nor sends would never be probed. Give
    // such a stall the same budget as an unanswered Ping. The two budgets are
    // compared in stages: their sum can overflow for long configured values.
    const auto idle_for = now - last_active_ms;
    if (write_active && idle_for >= ping_interval && idle_for - ping_interval >= pong_timeout) {
        return websocket_liveness_decision::abort_transport;
    }
    if (!std::holds_alternative<websocket_liveness_idle>(state_value)) {
        return websocket_liveness_decision::idle;
    }
    if (idle_for < ping_interval || write_active) {
        return websocket_liveness_decision::idle;
    }
    return websocket_liveness_decision::send_ping;
}

// A stalled writer's deadline is last activity + ping interval + pong timeout,
// without overflowing that sum or restarting the pong portion on intermediate
// timer ticks.
[[nodiscard]] inline std::chrono::milliseconds websocket_heartbeat_delay(
    const websocket_lifecycle_options& options, const websocket_liveness_state_type& state,
    bool writing, std::int64_t last_active_ms, std::int64_t now) noexcept {
    const auto remaining = [now](std::int64_t started, std::int64_t budget) {
        const auto elapsed = std::max<std::int64_t>(0, now - started);
        return elapsed >= budget ? std::int64_t{0} : budget - elapsed;
    };
    std::int64_t delay = 1;
    if (writing) {
        const auto elapsed = std::max<std::int64_t>(0, now - last_active_ms);
        const auto ping = options.heartbeat_.ping_interval_->count();
        const auto pong = options.heartbeat_.pong_timeout_->count();
        if (elapsed >= ping) {
            delay = elapsed - ping >= pong ? 0 : pong - (elapsed - ping);
        } else {
            const auto until_ping = ping - elapsed;
            delay = pong > std::numeric_limits<std::int64_t>::max() - until_ping
                        ? std::numeric_limits<std::int64_t>::max()
                        : until_ping + pong;
        }
    } else if (const auto* pong = std::get_if<websocket_awaiting_pong>(&state)) {
        delay = remaining(pong->sent_at_ms(), options.heartbeat_.pong_timeout_->count());
    } else if (std::holds_alternative<websocket_liveness_idle>(state)) {
        delay = remaining(last_active_ms, options.heartbeat_.ping_interval_->count());
    }
    return std::chrono::milliseconds(std::max<std::int64_t>(1, delay));
}

class websocket_peer_close_info final {
public:
    void record(const websocket_close_event& close_event) noexcept {
        code_ = close_event.close_code();
        const auto reason_value = close_event.reason();
        const auto count = (std::min)(reason_value.size(), reason_.size());
        if (count != 0) {
            std::char_traits<char>::copy(reason_.data(), reason_value.data(), count);
        }
        reason_size_ = static_cast<std::uint8_t>(count);
    }

    [[nodiscard]] std::optional<std::uint16_t> code() const noexcept {
        return code_;
    }

    [[nodiscard]] std::string_view reason() const& noexcept RUVIA_LIFETIMEBOUND {
        return {reason_.data(), reason_size_};
    }
    std::string_view reason() const&& = delete;

private:
    std::optional<std::uint16_t> code_{};
    std::array<char, 123> reason_{};
    std::uint8_t reason_size_{0};
};

class websocket_write_lane final {
public:
    explicit websocket_write_lane(const worker_handle& worker_value)
        : signal_(worker_value) {}
    websocket_write_lane(worker_handle&&) = delete;

    [[nodiscard]] bool write_in_progress() const noexcept {
        return write_in_progress_;
    }

    [[nodiscard]] bool reader_waiting() const noexcept {
        return reader_waiting_;
    }

    [[nodiscard]] bool blocked_for_writer() const noexcept {
        return write_in_progress_ || reader_waiting_;
    }

    void begin() noexcept {
        if (write_in_progress_) {
            std::terminate();
        }
        write_in_progress_ = true;
    }

    void release() noexcept {
        if (!write_in_progress_) {
            std::terminate();
        }
        write_in_progress_ = false;
    }

    void finish() noexcept {
        release();
        signal_.notify();
    }

    void notify() noexcept {
        signal_.notify();
    }

    [[nodiscard]] worker_signal::wait_operation wait() {
        return signal_.wait();
    }

    template <typename check_type>
    [[nodiscard]] task<void> wait_for_idle(bool for_reader, check_type check) {
        try {
            for (;;) {
                check();
                if (!write_in_progress_ && (for_reader || !reader_waiting_)) {
                    if (for_reader) {
                        // The caller claims the lane before any writer resumes.
                        reader_waiting_ = false;
                    }
                    co_return;
                }
                if (for_reader) {
                    reader_waiting_ = true;
                }
                co_await signal_.wait();
            }
        } catch (...) {
            if (for_reader) {
                reader_waiting_ = false;
                signal_.notify();
            }
            throw;
        }
    }

    [[nodiscard]] task<void> wait_for_idle(bool for_reader) {
        return wait_for_idle(for_reader, []() noexcept {});
    }

private:
    bool write_in_progress_{false};
    bool reader_waiting_{false};
    worker_signal signal_;
};

}  // namespace ruvia::detail
