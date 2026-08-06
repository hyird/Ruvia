#pragma once

#include "ruvia/http/HttpHeader.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/http/StreamingAccess.h"
#include "ruvia/core/Task.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/web/Streaming.h"

namespace ruvia {
class Context;  // only used as Context* in a type-erased bind thunk; web supplies the definition
}

namespace ruvia::detail {

template <typename Sink>
Task<void> responseStreamWriteThunk(void* target, std::string_view chunk) {
    co_await static_cast<Sink*>(target)->write(chunk);
}

template <typename Sink>
Task<void> responseStreamEndThunk(void* target, std::span<const HttpHeaderView> trailers) {
    co_await static_cast<Sink*>(target)->end(trailers);
}

template <typename Sink>
Task<TimerSleepResult> responseStreamSleepThunk(void* target, std::chrono::milliseconds duration) {
    co_return co_await static_cast<Sink*>(target)->sleep(duration);
}

template <typename Sink>
bool responseStreamAbortedThunk(void* target) noexcept {
    return static_cast<Sink*>(target)->aborted();
}

template <typename Sink>
void responseStreamBindContextThunk(void* target, Context* context, HttpResponse (*streamingHead)(Context&)) {
    static_cast<Sink*>(target)->bindContext(context, streamingHead);
}

template <typename Sink>
void responseStreamReleaseContextThunk(void* target) noexcept {
    static_cast<Sink*>(target)->releaseContext();
}

template <typename Sink>
bool responseStreamCommittedThunk(void* target) noexcept {
    return static_cast<Sink*>(target)->committed();
}

template <typename Sink>
[[nodiscard]] ResponseStreamWriter makeResponseStreamWriter(Sink& sink) noexcept {
    return StreamingAccess::makeResponseStreamWriter(&sink, &responseStreamWriteThunk<Sink>, &responseStreamEndThunk<Sink>, &responseStreamSleepThunk<Sink>, &responseStreamBindContextThunk<Sink>, &responseStreamReleaseContextThunk<Sink>, &responseStreamCommittedThunk<Sink>, &responseStreamAbortedThunk<Sink>);
}

class ResponseStreamCompleted final {
public:
    [[nodiscard]] constexpr HttpStatusCode status() const noexcept {
        return status_;
    }

private:
    friend class ResponseStreamDispatchResult;

    explicit constexpr ResponseStreamCompleted(HttpStatusCode status) noexcept
        : status_(status) {}

    HttpStatusCode status_;
};

class ResponseStreamPeerAbortedBeforeCommit final {
private:
    friend class ResponseStreamDispatchResult;

    constexpr ResponseStreamPeerAbortedBeforeCommit() noexcept = default;
};

class ResponseStreamPeerAbortedAfterCommit final {
public:
    [[nodiscard]] constexpr HttpStatusCode status() const noexcept {
        return status_;
    }

private:
    friend class ResponseStreamDispatchResult;

    explicit constexpr ResponseStreamPeerAbortedAfterCommit(HttpStatusCode status) noexcept
        : status_(status) {}

    HttpStatusCode status_;
};

// The handler failed after its head was already on the wire. The status is what
// the peer was told; the exception is why it will never receive the rest. The
// status alone cannot be reported to anyone -- it says 200 -- so the exception
// travels with it to the transport, which is the last owner able to report it.
class ResponseStreamFailedAfterCommit final {
public:
    [[nodiscard]] constexpr HttpStatusCode status() const noexcept {
        return status_;
    }

    // Never null.
    [[nodiscard]] std::exception_ptr exception() const noexcept {
        return exception_;
    }

private:
    friend class ResponseStreamDispatchResult;

    ResponseStreamFailedAfterCommit(HttpStatusCode status, std::exception_ptr exception) noexcept
        : status_(status),
          exception_(std::move(exception)) {}

    HttpStatusCode status_;
    std::exception_ptr exception_;
};

class ResponseStreamRouteResponse final {
public:
    [[nodiscard]] HttpResponse takeResponse() && noexcept {
        return std::move(response_);
    }

private:
    friend class ResponseStreamDispatchResult;

    explicit ResponseStreamRouteResponse(HttpResponse response) noexcept
        : response_(std::move(response)) {}

    HttpResponse response_;
};

class ResponseStreamRecoveredFailure final {
public:
    [[nodiscard]] HttpResponse takeResponse() && noexcept {
        return std::move(response_);
    }

private:
    friend class ResponseStreamDispatchResult;

    explicit ResponseStreamRecoveredFailure(HttpResponse response) noexcept
        : response_(std::move(response)) {}

    HttpResponse response_;
};

// Each route terminal is one alternative. Commit-bearing alternatives own the
// exact wire status, response-bearing alternatives own the only movable response,
// and a pre-commit peer abort owns neither. HTTP/1 and HTTP/2 need no second
// outcome discriminator.
class ResponseStreamDispatchResult final {
public:
    [[nodiscard]] static ResponseStreamDispatchResult makeCompleted(HttpStatusCode status) noexcept {
        return ResponseStreamDispatchResult(ResponseStreamCompleted(status));
    }

    [[nodiscard]] static ResponseStreamDispatchResult makePeerAbortedBeforeCommit() noexcept {
        return ResponseStreamDispatchResult(ResponseStreamPeerAbortedBeforeCommit{});
    }

    [[nodiscard]] static ResponseStreamDispatchResult makePeerAbortedAfterCommit(HttpStatusCode status) noexcept {
        return ResponseStreamDispatchResult(ResponseStreamPeerAbortedAfterCommit(status));
    }

    [[nodiscard]] static ResponseStreamDispatchResult makeFailedAfterCommit(HttpStatusCode status, std::exception_ptr exception) noexcept {
        return ResponseStreamDispatchResult(ResponseStreamFailedAfterCommit(status, std::move(exception)));
    }

    [[nodiscard]] static ResponseStreamDispatchResult makeRouteResponse(HttpResponse response) noexcept {
        return ResponseStreamDispatchResult(ResponseStreamRouteResponse(std::move(response)));
    }

    [[nodiscard]] static ResponseStreamDispatchResult makeRecoveredFailure(HttpResponse response) noexcept {
        return ResponseStreamDispatchResult(ResponseStreamRecoveredFailure(std::move(response)));
    }

    [[nodiscard]] const ResponseStreamCompleted* completed() const& noexcept {
        return std::get_if<ResponseStreamCompleted>(&value_);
    }
    const ResponseStreamCompleted* completed() const&& = delete;

    [[nodiscard]] const ResponseStreamPeerAbortedBeforeCommit* peerAbortedBeforeCommit() const& noexcept {
        return std::get_if<ResponseStreamPeerAbortedBeforeCommit>(&value_);
    }
    [[nodiscard]] const ResponseStreamPeerAbortedBeforeCommit* peerAbortedBeforeCommit() const&& = delete;

    [[nodiscard]] const ResponseStreamPeerAbortedAfterCommit* peerAbortedAfterCommit() const& noexcept {
        return std::get_if<ResponseStreamPeerAbortedAfterCommit>(&value_);
    }
    const ResponseStreamPeerAbortedAfterCommit* peerAbortedAfterCommit() const&& = delete;

    [[nodiscard]] const ResponseStreamFailedAfterCommit* failedAfterCommit() const& noexcept {
        return std::get_if<ResponseStreamFailedAfterCommit>(&value_);
    }
    const ResponseStreamFailedAfterCommit* failedAfterCommit() const&& = delete;

    [[nodiscard]] ResponseStreamRouteResponse* routeResponse() & noexcept {
        return std::get_if<ResponseStreamRouteResponse>(&value_);
    }
    ResponseStreamRouteResponse* routeResponse() && = delete;

    [[nodiscard]] ResponseStreamRecoveredFailure* recoveredFailure() & noexcept {
        return std::get_if<ResponseStreamRecoveredFailure>(&value_);
    }
    ResponseStreamRecoveredFailure* recoveredFailure() && = delete;

    [[nodiscard]] std::optional<HttpStatusCode> committedStatus() const noexcept {
        if (const auto* value = completed()) {
            return value->status();
        }
        if (const auto* value = peerAbortedAfterCommit()) {
            return value->status();
        }
        if (const auto* value = failedAfterCommit()) {
            return value->status();
        }
        return std::nullopt;
    }

private:
    using Value = std::variant<ResponseStreamCompleted, ResponseStreamPeerAbortedBeforeCommit, ResponseStreamPeerAbortedAfterCommit, ResponseStreamFailedAfterCommit, ResponseStreamRouteResponse, ResponseStreamRecoveredFailure>;

    template <typename Alternative>
    explicit ResponseStreamDispatchResult(Alternative alternative) noexcept
        : value_(std::move(alternative)) {}

    Value value_;
};

template <typename Sink>
[[nodiscard]] HttpStatusCode committedResponseStreamStatus(const Sink& sink) {
    const auto* plan = sink.commitPlan();
    if (plan == nullptr) {
        throw std::logic_error("response stream has no committed protocol plan");
    }
    return plan->responseStatus();
}

// Drives a response-stream route over an already-constructed sink. peerAborted
// is a predicate consulted after the handler returns so a transport that can be
// reset out from under the handler (HTTP/2, where the reset flag is a bit-field)
// can distinguish an abort before any final head from one after the commit plan
// exists; HTTP/1 passes a constant-false predicate, which folds away. Connection
// persistence remains a transport concern after this helper returns a buffered
// pre-commit error response.
template <typename Sink, typename PeerAborted>
Task<ResponseStreamDispatchResult> dispatchResponseStreamWith(Sink& sink, const RouteTable& routes, const HttpRequest& request, const ResolvedRoute& route, RequestMemory& requestMemory, ContextServices services, PeerAborted peerAborted) {
    auto responseStream = makeResponseStreamWriter(sink);

    std::exception_ptr exception;
    try {
        auto result = co_await routes.dispatchResponseStream(request, route, requestMemory, responseStream, services);
        if (peerAborted()) {
            if (!sink.committed()) {
                co_return ResponseStreamDispatchResult::makePeerAbortedBeforeCommit();
            }
            co_return ResponseStreamDispatchResult::makePeerAbortedAfterCommit(committedResponseStreamStatus(sink));
        }
        if (!result.has_value()) {
            if (!sink.committed()) {
                throw std::logic_error("handled response stream has no committed protocol plan");
            }
            co_return ResponseStreamDispatchResult::makeCompleted(committedResponseStreamStatus(sink));
        }
        if (sink.committed()) {
            throw std::logic_error("buffered stream result followed a committed response head");
        }
        co_return ResponseStreamDispatchResult::makeRouteResponse(std::move(*result));
    } catch (...) {
        exception = std::current_exception();
    }

    if (exception == nullptr) {
        throw std::logic_error("response stream dispatch left no terminal result");
    }
    if (peerAborted()) {
        if (!sink.committed()) {
            co_return ResponseStreamDispatchResult::makePeerAbortedBeforeCommit();
        }
        co_return ResponseStreamDispatchResult::makePeerAbortedAfterCommit(committedResponseStreamStatus(sink));
    }
    if (sink.committed()) {
        // Past the point of no return: the status cannot be changed and no
        // error body can be appended, so the exception is handed to the
        // transport rather than dropped with the connection.
        co_return ResponseStreamDispatchResult::makeFailedAfterCommit(committedResponseStreamStatus(sink), std::move(exception));
    }
    auto response = co_await routes.handleException(request, requestMemory, exception, services);
    co_return ResponseStreamDispatchResult::makeRecoveredFailure(std::move(response));
}

}  // namespace ruvia::detail
