#pragma once

#include <cstdint>
#include <variant>

#include "http2/http2_frame_types.h"

namespace ruvia::detail {

class http2_local_connection_open final {};

// A server drains in two phases (RFC 9113 section 6.8). During the round trip the
// advertised boundary is 2^31-1 and peer streams are still admitted up to
// round_trip_stream_limit(); after the PING ACK (or that limit) the boundary is the
// real last peer stream id and is final. A client drains in the final phase at once.
class http2_local_connection_graceful_drain final {
public:
    [[nodiscard]] constexpr std::uint32_t last_stream_id() const noexcept {
        return last_stream_id_;
    }
    [[nodiscard]] constexpr bool awaiting_round_trip() const noexcept {
        return awaiting_round_trip_;
    }
    // Highest peer stream id still admitted while awaiting the round trip.
    [[nodiscard]] constexpr std::uint32_t round_trip_stream_limit() const noexcept {
        return round_trip_stream_limit_;
    }

private:
    friend class http2_local_connection_state;

    constexpr http2_local_connection_graceful_drain(std::uint32_t last_stream_id,
        bool awaiting_round_trip, std::uint32_t round_trip_stream_limit) noexcept
        : last_stream_id_(last_stream_id),
          round_trip_stream_limit_(round_trip_stream_limit),
          awaiting_round_trip_(awaiting_round_trip) {}

    std::uint32_t last_stream_id_;
    std::uint32_t round_trip_stream_limit_;
    bool awaiting_round_trip_;
};

class http2_local_connection_fatal_failure final {
public:
    [[nodiscard]] constexpr http2_error_code error() const noexcept {
        return error_;
    }

private:
    friend class http2_local_connection_state;

    explicit constexpr http2_local_connection_fatal_failure(http2_error_code error) noexcept
        : error_(error) {}

    http2_error_code error_;
};

// The locally initiated connection lifecycle owns its GOAWAY meaning. A graceful
// drain alone carries the advertised stream boundary; a fatal protocol failure
// alone carries an error code and atomically supersedes any earlier drain.
// Peer GOAWAY and preface progress are directional/orthogonal state elsewhere.
class http2_local_connection_state final {
public:
    [[nodiscard]] constexpr const http2_local_connection_open* open() const& noexcept {
        return std::get_if<http2_local_connection_open>(&state_);
    }
    const http2_local_connection_open* open() const&& = delete;

    [[nodiscard]] constexpr const http2_local_connection_graceful_drain* graceful_drain()
        const& noexcept {
        return std::get_if<http2_local_connection_graceful_drain>(&state_);
    }
    const http2_local_connection_graceful_drain* graceful_drain() const&& = delete;

    [[nodiscard]] constexpr const http2_local_connection_fatal_failure* fatal_failure() const& noexcept {
        return std::get_if<http2_local_connection_fatal_failure>(&state_);
    }
    const http2_local_connection_fatal_failure* fatal_failure() const&& = delete;

    [[nodiscard]] bool begin_graceful_drain(std::uint32_t last_stream_id) noexcept {
        if (open() == nullptr) {
            return false;
        }
        state_ = state_type(http2_local_connection_graceful_drain(last_stream_id, false, last_stream_id));
        return true;
    }

    [[nodiscard]] bool begin_round_trip_drain(
        std::uint32_t advertised_stream_id, std::uint32_t round_trip_stream_limit) noexcept {
        if (open() == nullptr) {
            return false;
        }
        state_ = state_type(
            http2_local_connection_graceful_drain(advertised_stream_id, true, round_trip_stream_limit));
        return true;
    }

    [[nodiscard]] bool finish_round_trip_drain(std::uint32_t last_stream_id) noexcept {
        const auto* drain = graceful_drain();
        if (drain == nullptr || !drain->awaiting_round_trip()) {
            return false;
        }
        state_ = state_type(http2_local_connection_graceful_drain(last_stream_id, false, last_stream_id));
        return true;
    }

    void fail(http2_error_code error) noexcept {
        state_ = state_type(http2_local_connection_fatal_failure(error));
    }

private:
    using state_type = std::variant<http2_local_connection_open, http2_local_connection_graceful_drain,
        http2_local_connection_fatal_failure>;

    state_type state_;
};

}  // namespace ruvia::detail
