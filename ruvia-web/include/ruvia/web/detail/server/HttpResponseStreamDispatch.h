#pragma once

#include <chrono>
#include <cstdint>
#include <exception>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/http/StreamingInternal.h"
#include "ruvia/core/Task.h"
#include "ruvia/http/HttpTypes.h"
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
Task<void> responseStreamEndThunk(
    void* target,
    std::span<const HttpHeaderView> trailers) {
    co_await static_cast<Sink*>(target)->end(trailers);
}

template <typename Sink>
Task<void> responseStreamSleepThunk(void* target, std::chrono::milliseconds duration) {
    co_await static_cast<Sink*>(target)->sleep(duration);
}

template <typename Sink>
bool responseStreamAbortedThunk(void* target) noexcept {
    return static_cast<Sink*>(target)->aborted();
}

template <typename Sink>
void responseStreamBindContextThunk(
    void* target, Context* context, HttpResponse (*streamingHead)(Context&)) noexcept {
    static_cast<Sink*>(target)->bindContext(context, streamingHead);
}

template <typename Sink>
std::pmr::string& responseStreamScratchThunk(void* target) noexcept {
    return static_cast<Sink*>(target)->scratch();
}

template <typename Sink>
bool responseStreamCommittedThunk(void* target) noexcept {
    return static_cast<Sink*>(target)->committed();
}

template <typename Sink>
[[nodiscard]] ResponseStreamWriter makeResponseStreamWriter(Sink& sink) noexcept {
    return StreamingAccess::makeResponseStreamWriter(
        &sink,
        &responseStreamWriteThunk<Sink>,
        &responseStreamEndThunk<Sink>,
        &responseStreamSleepThunk<Sink>,
        &responseStreamBindContextThunk<Sink>,
        &responseStreamScratchThunk<Sink>,
        &responseStreamCommittedThunk<Sink>,
        &responseStreamAbortedThunk<Sink>);
}

class ResponseStreamCompleted final {
public:
    [[nodiscard]] constexpr std::uint16_t status() const noexcept {
        return status_;
    }

private:
    friend class ResponseStreamDispatchResult;

    explicit constexpr ResponseStreamCompleted(std::uint16_t status) noexcept
        : status_(status) {}

    std::uint16_t status_;
};

class ResponseStreamPeerAbortedBeforeCommit final {
private:
    friend class ResponseStreamDispatchResult;

    constexpr ResponseStreamPeerAbortedBeforeCommit() noexcept = default;
};

class ResponseStreamPeerAbortedAfterCommit final {
public:
    [[nodiscard]] constexpr std::uint16_t status() const noexcept {
        return status_;
    }

private:
    friend class ResponseStreamDispatchResult;

    explicit constexpr ResponseStreamPeerAbortedAfterCommit(
        std::uint16_t status) noexcept
        : status_(status) {}

    std::uint16_t status_;
};

class ResponseStreamFailedAfterCommit final {
public:
    [[nodiscard]] constexpr std::uint16_t status() const noexcept {
        return status_;
    }

private:
    friend class ResponseStreamDispatchResult;

    explicit constexpr ResponseStreamFailedAfterCommit(
        std::uint16_t status) noexcept
        : status_(status) {}

    std::uint16_t status_;
};

class ResponseStreamBuffered final {
public:
    [[nodiscard]] HttpResponse takeResponse() && noexcept {
        return std::move(response_);
    }

private:
    friend class ResponseStreamDispatchResult;

    explicit ResponseStreamBuffered(HttpResponse response) noexcept
        : response_(std::move(response)) {}

    HttpResponse response_;
};

class ResponseStreamFailedBeforeCommit final {
public:
    [[nodiscard]] HttpResponse takeResponse() && noexcept {
        return std::move(response_);
    }

private:
    friend class ResponseStreamDispatchResult;

    explicit ResponseStreamFailedBeforeCommit(HttpResponse response) noexcept
        : response_(std::move(response)) {}

    HttpResponse response_;
};

// Every terminal stream-dispatch state owns exactly the payload valid for that
// state. In particular, a committed state carries the exact status from the
// protocol commit plan, while a pre-commit peer abort owns no fictitious response.
class ResponseStreamDispatchResult final {
public:
    [[nodiscard]] static ResponseStreamDispatchResult makeCompleted(
        std::uint16_t status) noexcept {
        return ResponseStreamDispatchResult(ResponseStreamCompleted(status));
    }

    [[nodiscard]] static ResponseStreamDispatchResult
    makePeerAbortedBeforeCommit() noexcept {
        return ResponseStreamDispatchResult(
            ResponseStreamPeerAbortedBeforeCommit{});
    }

    [[nodiscard]] static ResponseStreamDispatchResult
    makePeerAbortedAfterCommit(std::uint16_t status) noexcept {
        return ResponseStreamDispatchResult(
            ResponseStreamPeerAbortedAfterCommit(status));
    }

    [[nodiscard]] static ResponseStreamDispatchResult makeFailedAfterCommit(
        std::uint16_t status) noexcept {
        return ResponseStreamDispatchResult(
            ResponseStreamFailedAfterCommit(status));
    }

    [[nodiscard]] static ResponseStreamDispatchResult makeBuffered(
        HttpResponse response) noexcept {
        return ResponseStreamDispatchResult(
            ResponseStreamBuffered(std::move(response)));
    }

    [[nodiscard]] static ResponseStreamDispatchResult makeFailedBeforeCommit(
        HttpResponse response) noexcept {
        return ResponseStreamDispatchResult(
            ResponseStreamFailedBeforeCommit(std::move(response)));
    }

    [[nodiscard]] const ResponseStreamCompleted* completed() const noexcept {
        return std::get_if<ResponseStreamCompleted>(&value_);
    }

    [[nodiscard]] const ResponseStreamPeerAbortedBeforeCommit*
    peerAbortedBeforeCommit() const noexcept {
        return std::get_if<ResponseStreamPeerAbortedBeforeCommit>(&value_);
    }

    [[nodiscard]] const ResponseStreamPeerAbortedAfterCommit*
    peerAbortedAfterCommit() const noexcept {
        return std::get_if<ResponseStreamPeerAbortedAfterCommit>(&value_);
    }

    [[nodiscard]] const ResponseStreamFailedAfterCommit*
    failedAfterCommit() const noexcept {
        return std::get_if<ResponseStreamFailedAfterCommit>(&value_);
    }

    [[nodiscard]] const ResponseStreamBuffered* buffered() const noexcept {
        return std::get_if<ResponseStreamBuffered>(&value_);
    }

    [[nodiscard]] ResponseStreamBuffered* buffered() noexcept {
        return std::get_if<ResponseStreamBuffered>(&value_);
    }

    [[nodiscard]] const ResponseStreamFailedBeforeCommit*
    failedBeforeCommit() const noexcept {
        return std::get_if<ResponseStreamFailedBeforeCommit>(&value_);
    }

    [[nodiscard]] ResponseStreamFailedBeforeCommit*
    failedBeforeCommit() noexcept {
        return std::get_if<ResponseStreamFailedBeforeCommit>(&value_);
    }

private:
    using Value = std::variant<
        ResponseStreamCompleted,
        ResponseStreamPeerAbortedBeforeCommit,
        ResponseStreamPeerAbortedAfterCommit,
        ResponseStreamFailedAfterCommit,
        ResponseStreamBuffered,
        ResponseStreamFailedBeforeCommit>;

    template <typename Alternative>
    explicit ResponseStreamDispatchResult(Alternative alternative) noexcept
        : value_(std::move(alternative)) {}

    Value value_;
};

template <typename Sink>
[[nodiscard]] std::uint16_t committedResponseStreamStatus(
    const Sink& sink) {
    const auto* plan = sink.commitPlan();
    if (plan == nullptr) {
        throw std::logic_error(
            "response stream has no committed protocol plan");
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
Task<ResponseStreamDispatchResult> dispatchResponseStreamWith(
    Sink& sink,
    const RouteTable& routes,
    const HttpRequest& request,
    const ResolvedRoute& route,
    RequestMemory& requestMemory,
    ContextServices services,
    PeerAborted peerAborted) {
    auto responseStream = makeResponseStreamWriter(sink);

    std::exception_ptr exception;
    try {
        auto result = co_await routes.dispatchResponseStream(
            request, route, requestMemory, responseStream, services);
        if (peerAborted()) {
            if (!sink.committed()) {
                co_return ResponseStreamDispatchResult::
                    makePeerAbortedBeforeCommit();
            }
            co_return ResponseStreamDispatchResult::makePeerAbortedAfterCommit(
                committedResponseStreamStatus(sink));
        }
        if (result.handled() != nullptr) {
            if (!sink.committed()) {
                throw std::logic_error(
                    "handled response stream has no committed protocol plan");
            }
            co_return ResponseStreamDispatchResult::makeCompleted(
                committedResponseStreamStatus(sink));
        }
        if (sink.committed()) {
            throw std::logic_error(
                "buffered stream result followed a committed response head");
        }
        auto* buffered = result.buffered();
        if (buffered == nullptr) {
            throw std::logic_error(
                "stream route returned no terminal alternative");
        }
        co_return ResponseStreamDispatchResult::makeBuffered(
            std::move(*buffered).takeResponse());
    } catch (...) {
        exception = std::current_exception();
    }

    if (exception == nullptr) {
        throw std::logic_error(
            "response stream dispatch left no terminal result");
    }
    if (peerAborted()) {
        if (!sink.committed()) {
            co_return ResponseStreamDispatchResult::
                makePeerAbortedBeforeCommit();
        }
        co_return ResponseStreamDispatchResult::makePeerAbortedAfterCommit(
            committedResponseStreamStatus(sink));
    }
    if (sink.committed()) {
        co_return ResponseStreamDispatchResult::makeFailedAfterCommit(
            committedResponseStreamStatus(sink));
    }
    auto response = co_await routes.handleException(
        request, requestMemory, exception, services);
    co_return ResponseStreamDispatchResult::makeFailedBeforeCommit(
        std::move(response));
}

}  // namespace ruvia::detail
