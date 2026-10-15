#pragma once

#include <cstdint>
#include <variant>

#include "http2/http2_frame_types.h"

namespace ruvia::detail {

class http2_local_connection_open final {};

class http2_local_connection_graceful_drain final {
public:
    [[nodiscard]] constexpr std::uint32_t last_stream_id() const noexcept {
        return last_stream_id_;
    }

private:
    friend class http2_local_connection_state;

    explicit constexpr http2_local_connection_graceful_drain(std::uint32_t last_stream_id) noexcept
        : last_stream_id_(last_stream_id) {}

    std::uint32_t last_stream_id_;
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
        state_ = state_type(http2_local_connection_graceful_drain(last_stream_id));
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
