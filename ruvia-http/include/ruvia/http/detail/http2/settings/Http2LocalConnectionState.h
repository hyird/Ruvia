#pragma once

#include <cstdint>
#include <variant>

#include "ruvia/http/detail/http2/frame/Http2FrameTypes.h"

namespace ruvia::detail {

class Http2LocalConnectionOpen final {};

class Http2LocalConnectionGracefulDrain final {
public:
    [[nodiscard]] constexpr std::uint32_t lastStreamId() const noexcept {
        return lastStreamId_;
    }

private:
    friend class Http2LocalConnectionState;

    explicit constexpr Http2LocalConnectionGracefulDrain(
        std::uint32_t lastStreamId) noexcept
        : lastStreamId_(lastStreamId) {}

    std::uint32_t lastStreamId_;
};

class Http2LocalConnectionFatalFailure final {
public:
    [[nodiscard]] constexpr Http2ErrorCode error() const noexcept {
        return error_;
    }

private:
    friend class Http2LocalConnectionState;

    explicit constexpr Http2LocalConnectionFatalFailure(
        Http2ErrorCode error) noexcept
        : error_(error) {}

    Http2ErrorCode error_;
};

// The locally initiated connection lifecycle owns its GOAWAY meaning. A graceful
// drain alone carries the advertised stream boundary; a fatal protocol failure
// alone carries an error code and atomically supersedes any earlier drain.
// Peer GOAWAY and preface progress are directional/orthogonal state elsewhere.
class Http2LocalConnectionState final {
public:
    [[nodiscard]] constexpr const Http2LocalConnectionOpen*
    open() const & noexcept {
        return std::get_if<Http2LocalConnectionOpen>(&state_);
    }
    const Http2LocalConnectionOpen* open() const && = delete;

    [[nodiscard]] constexpr const Http2LocalConnectionGracefulDrain*
    gracefulDrain() const & noexcept {
        return std::get_if<Http2LocalConnectionGracefulDrain>(&state_);
    }
    const Http2LocalConnectionGracefulDrain* gracefulDrain() const && = delete;

    [[nodiscard]] constexpr const Http2LocalConnectionFatalFailure*
    fatalFailure() const & noexcept {
        return std::get_if<Http2LocalConnectionFatalFailure>(&state_);
    }
    const Http2LocalConnectionFatalFailure* fatalFailure() const && = delete;

    [[nodiscard]] bool beginGracefulDrain(
        std::uint32_t lastStreamId) noexcept {
        if (open() == nullptr) {
            return false;
        }
        state_ = State(Http2LocalConnectionGracefulDrain(lastStreamId));
        return true;
    }

    void fail(Http2ErrorCode error) noexcept {
        state_ = State(Http2LocalConnectionFatalFailure(error));
    }

private:
    using State = std::variant<
        Http2LocalConnectionOpen,
        Http2LocalConnectionGracefulDrain,
        Http2LocalConnectionFatalFailure>;

    State state_;
};

}  // namespace ruvia::detail
