#pragma once

#include <cstdint>
#include <type_traits>

#include "ruvia/core/Task.h"

namespace ruvia::detail {

class Http2Connection;
class Http2SansIoStreamSignal;

class Http2SendWindowReady final {
private:
    constexpr Http2SendWindowReady() noexcept = default;
    friend class Http2SendWindowWaitResult;
};

class Http2SendWindowAborted final {
private:
    constexpr Http2SendWindowAborted() noexcept = default;
    friend class Http2SendWindowWaitResult;
};

class Http2SendWindowWaitResult final {
public:
    [[nodiscard]] static constexpr Http2SendWindowWaitResult makeReady() noexcept {
        return Http2SendWindowWaitResult(State::kReady);
    }

    [[nodiscard]] static constexpr Http2SendWindowWaitResult makeAborted() noexcept {
        return Http2SendWindowWaitResult(State::kAborted);
    }

    [[nodiscard]] constexpr const Http2SendWindowReady* ready() const& noexcept {
        return state_ == State::kReady ? &kReady : nullptr;
    }
    const Http2SendWindowReady* ready() const&& = delete;

    [[nodiscard]] constexpr const Http2SendWindowAborted* aborted() const& noexcept {
        return state_ == State::kAborted ? &kAborted : nullptr;
    }
    const Http2SendWindowAborted* aborted() const&& = delete;

private:
    enum class State : std::uint8_t { kReady,
        kAborted };

    explicit constexpr Http2SendWindowWaitResult(State state) noexcept
        : state_(state) {}

    static inline constexpr Http2SendWindowReady kReady{};
    static inline constexpr Http2SendWindowAborted kAborted{};

    State state_;
};

static_assert(std::is_trivially_copyable_v<Http2SendWindowWaitResult>);
static_assert(sizeof(Http2SendWindowWaitResult) <= 2);

// Wait until the HTTP core no longer owns queued DATA for this stream. The
// Web-owned signal is only a wakeup edge; stream existence, abort, session end,
// and queue state are re-checked together after every wake.
[[nodiscard]] Task<Http2SendWindowWaitResult> awaitHttp2SendWindow(
    Http2Connection& connection, std::uint32_t streamId, Http2SansIoStreamSignal* signal);

}  // namespace ruvia::detail
