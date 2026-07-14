#pragma once

#include <utility>
#include <variant>

#include "ruvia/http/HttpResponse.h"

namespace ruvia::detail {

// One chain-owned bit records that `next()` reached the stream/WebSocket
// handler even when it emitted no bytes. RouteTable is the sole mutator; Next
// carries only a typed pointer to this state, so continuation dispatch has no
// erased outcome channel or extra request-time storage.
class StreamMiddlewareChainState final {
public:
    [[nodiscard]] constexpr bool handlerInvoked() const noexcept {
        return handlerInvoked_;
    }

private:
    friend class RouteTable;

    constexpr StreamMiddlewareChainState() noexcept = default;

    constexpr void markHandlerInvoked() noexcept {
        handlerInvoked_ = true;
    }

    bool handlerInvoked_{false};
};

static_assert(sizeof(StreamMiddlewareChainState) == sizeof(bool));

class StreamRouteHandled final {
private:
    friend class StreamDispatchResult;

    constexpr StreamRouteHandled() noexcept = default;
};

class StreamRouteBufferedResponse final {
public:
    [[nodiscard]] HttpResponse takeResponse() && noexcept {
        return std::move(response_);
    }

private:
    friend class StreamDispatchResult;

    explicit StreamRouteBufferedResponse(HttpResponse response) noexcept
        : response_(std::move(response)) {}

    HttpResponse response_;
};

// A stream route either handled its output on the bound writer/WebSocket or owns
// one buffered fallback response. No dummy HttpResponse exists in the handled
// alternative, so middleware cannot create an outcome/response mismatch.
class StreamDispatchResult final {
public:
    [[nodiscard]] static StreamDispatchResult makeHandled() noexcept {
        return StreamDispatchResult(StreamRouteHandled{});
    }

    [[nodiscard]] static StreamDispatchResult makeBuffered(
        HttpResponse response) noexcept {
        return StreamDispatchResult(
            StreamRouteBufferedResponse(std::move(response)));
    }

    [[nodiscard]] const StreamRouteHandled* handled() const & noexcept {
        return std::get_if<StreamRouteHandled>(&value_);
    }
    [[nodiscard]] const StreamRouteHandled* handled() const && = delete;

    [[nodiscard]] const StreamRouteBufferedResponse*
    buffered() const & noexcept {
        return std::get_if<StreamRouteBufferedResponse>(&value_);
    }
    [[nodiscard]] const StreamRouteBufferedResponse*
    buffered() const && = delete;

    [[nodiscard]] StreamRouteBufferedResponse* buffered() & noexcept {
        return std::get_if<StreamRouteBufferedResponse>(&value_);
    }
    [[nodiscard]] StreamRouteBufferedResponse* buffered() && = delete;

private:
    using Value = std::variant<
        StreamRouteHandled,
        StreamRouteBufferedResponse>;

    template <typename Alternative>
    explicit StreamDispatchResult(Alternative alternative) noexcept
        : value_(std::move(alternative)) {}

    Value value_;
};

}  // namespace ruvia::detail
