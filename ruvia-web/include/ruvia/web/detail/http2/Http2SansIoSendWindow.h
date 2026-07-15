#pragma once

#include <cstdint>
#include <type_traits>
#include <variant>

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
        return Http2SendWindowWaitResult(Http2SendWindowReady{});
    }

    [[nodiscard]] static constexpr Http2SendWindowWaitResult makeAborted() noexcept {
        return Http2SendWindowWaitResult(Http2SendWindowAborted{});
    }

    [[nodiscard]] constexpr const Http2SendWindowReady* ready() const & noexcept {
        return std::get_if<Http2SendWindowReady>(&value_);
    }
    const Http2SendWindowReady* ready() const && = delete;

    [[nodiscard]] constexpr const Http2SendWindowAborted* aborted() const & noexcept {
        return std::get_if<Http2SendWindowAborted>(&value_);
    }
    const Http2SendWindowAborted* aborted() const && = delete;

private:
    explicit constexpr Http2SendWindowWaitResult(
        Http2SendWindowReady ready) noexcept
        : value_(ready) {}

    explicit constexpr Http2SendWindowWaitResult(
        Http2SendWindowAborted aborted) noexcept
        : value_(aborted) {}

    std::variant<Http2SendWindowReady, Http2SendWindowAborted> value_;
};

static_assert(std::is_trivially_copyable_v<Http2SendWindowWaitResult>);
static_assert(sizeof(Http2SendWindowWaitResult) <= 2);

// Wait until the HTTP core no longer owns queued DATA for this stream. The
// Web-owned signal is only a wakeup edge; stream existence, abort, session end,
// and queue state are re-checked together after every wake.
[[nodiscard]] Task<Http2SendWindowWaitResult> awaitHttp2SendWindow(
    Http2Connection& connection,
    std::uint32_t streamId,
    Http2SansIoStreamSignal* signal);

}  // namespace ruvia::detail
