#pragma once

#include <utility>
#include <variant>

#include "ruvia/http/HttpResponse.h"

namespace ruvia::detail {

// Transient continuation state for the middleware chain. The final dispatch
// result below is discriminated independently and never stores this enum beside
// a response payload.
enum class StreamMiddlewareDisposition {
    kBufferedResponse,
    kStreamHandled
};

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

    [[nodiscard]] const StreamRouteHandled* handled() const noexcept {
        return std::get_if<StreamRouteHandled>(&value_);
    }

    [[nodiscard]] const StreamRouteBufferedResponse* buffered() const noexcept {
        return std::get_if<StreamRouteBufferedResponse>(&value_);
    }

    [[nodiscard]] StreamRouteBufferedResponse* buffered() noexcept {
        return std::get_if<StreamRouteBufferedResponse>(&value_);
    }

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
