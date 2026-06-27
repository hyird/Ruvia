#pragma once

#include "ruvia/http/HttpResponse.h"

#include <cstdint>
#include <optional>
#include <utility>

namespace ruvia::detail {

class Http2RouteDispatchResult final {
public:
    [[nodiscard]] static Http2RouteDispatchResult makeStreamHandled() noexcept {
        return Http2RouteDispatchResult(Outcome::kStreamHandled);
    }

    [[nodiscard]] static Http2RouteDispatchResult makeBufferedResponse(HttpResponse response) {
        return Http2RouteDispatchResult(std::move(response));
    }

    [[nodiscard]] bool streamHandled() const noexcept {
        return outcome_ == Outcome::kStreamHandled;
    }

    [[nodiscard]] bool bufferedResponse() const noexcept {
        return outcome_ == Outcome::kBufferedResponse;
    }

    [[nodiscard]] HttpResponse takeResponse() noexcept {
        return std::move(*response_);
    }

private:
    enum class Outcome {
        kStreamHandled,
        kBufferedResponse,
    };

    explicit Http2RouteDispatchResult(Outcome outcome) noexcept
        : outcome_(outcome) {}

    explicit Http2RouteDispatchResult(HttpResponse response)
        : outcome_(Outcome::kBufferedResponse),
          response_(std::move(response)) {}

    Outcome outcome_;
    std::optional<HttpResponse> response_;
};

class Http2SessionFlow final {
public:
    [[nodiscard]] static constexpr Http2SessionFlow keepRunning() noexcept {
        return Http2SessionFlow(State::kKeepRunning);
    }

    [[nodiscard]] static constexpr Http2SessionFlow stopRunning() noexcept {
        return Http2SessionFlow(State::kStopRunning);
    }

    [[nodiscard]] constexpr bool shouldContinue() const noexcept {
        return state_ == State::kKeepRunning;
    }

    [[nodiscard]] constexpr bool shouldStop() const noexcept {
        return state_ == State::kStopRunning;
    }

private:
    enum class State : std::uint8_t {
        kKeepRunning,
        kStopRunning,
    };

    constexpr explicit Http2SessionFlow(State state) noexcept
        : state_(state) {}

    State state_;
};

class Http2InputReadResult final {
public:
    [[nodiscard]] static constexpr Http2InputReadResult ready() noexcept {
        return Http2InputReadResult(State::kReady);
    }

    [[nodiscard]] static constexpr Http2InputReadResult stopReading() noexcept {
        return Http2InputReadResult(State::kStopReading);
    }

    [[nodiscard]] constexpr bool isReady() const noexcept {
        return state_ == State::kReady;
    }

    [[nodiscard]] constexpr bool shouldStop() const noexcept {
        return state_ == State::kStopReading;
    }

private:
    enum class State : std::uint8_t {
        kReady,
        kStopReading,
    };

    constexpr explicit Http2InputReadResult(State state) noexcept
        : state_(state) {}

    State state_;
};

class Http2DataWindowResult final {
public:
    [[nodiscard]] static constexpr Http2DataWindowResult ready() noexcept {
        return Http2DataWindowResult(State::kReady);
    }

    [[nodiscard]] static constexpr Http2DataWindowResult stopWriting() noexcept {
        return Http2DataWindowResult(State::kStopWriting);
    }

    [[nodiscard]] constexpr bool isReady() const noexcept {
        return state_ == State::kReady;
    }

    [[nodiscard]] constexpr bool shouldStop() const noexcept {
        return state_ == State::kStopWriting;
    }

private:
    enum class State : std::uint8_t {
        kReady,
        kStopWriting,
    };

    constexpr explicit Http2DataWindowResult(State state) noexcept
        : state_(state) {}

    State state_;
};

}  // namespace ruvia::detail
