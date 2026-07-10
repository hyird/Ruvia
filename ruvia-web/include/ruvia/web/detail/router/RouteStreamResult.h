#pragma once

#include <utility>

#include "ruvia/http/HttpResponse.h"

namespace ruvia::detail {

// Outcome of dispatching a streaming/websocket route: either the handler streamed
// the response itself (kStreamHandled) or it fell back to a buffered response the
// caller must still write (kBufferedResponse).
enum class RouteStreamDispatchOutcome {
    kBufferedResponse,
    kStreamHandled
};

class StreamDispatchResult final {
public:
    StreamDispatchResult(HttpResponse response, RouteStreamDispatchOutcome outcome)
        : response_(std::move(response)),
          outcome_(outcome) {}

    [[nodiscard]] bool streamHandled() const noexcept {
        return outcome_ == RouteStreamDispatchOutcome::kStreamHandled;
    }

    [[nodiscard]] bool bufferedResponse() const noexcept {
        return outcome_ == RouteStreamDispatchOutcome::kBufferedResponse;
    }

    [[nodiscard]] HttpResponse takeResponse() noexcept {
        return std::move(response_);
    }

private:
    HttpResponse response_;
    RouteStreamDispatchOutcome outcome_{RouteStreamDispatchOutcome::kBufferedResponse};
};

}  // namespace ruvia::detail
