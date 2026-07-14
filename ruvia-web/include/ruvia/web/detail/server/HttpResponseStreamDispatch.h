#pragma once

#include "ruvia/http/HttpHeader.h"

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

enum class ResponseStreamCommittedOutcome : std::uint8_t {
    kCompleted,
    kPeerAborted,
    kFailed
};

// Once the final head is committed, every terminal outcome owns the same wire
// fact: its exact status. The outcome only tells the protocol driver whether it
// must recover the transport before returning.
class ResponseStreamCommitted final {
public:
    [[nodiscard]] constexpr std::uint16_t status() const noexcept {
        return status_;
    }

    [[nodiscard]] constexpr ResponseStreamCommittedOutcome
    outcome() const noexcept {
        return outcome_;
    }

private:
    friend class ResponseStreamDispatchResult;

    constexpr ResponseStreamCommitted(
        std::uint16_t status,
        ResponseStreamCommittedOutcome outcome) noexcept
        : status_(status),
          outcome_(outcome) {}

    std::uint16_t status_;
    ResponseStreamCommittedOutcome outcome_;
};

class ResponseStreamPeerAbortedBeforeCommit final {
private:
    friend class ResponseStreamDispatchResult;

    constexpr ResponseStreamPeerAbortedBeforeCommit() noexcept = default;
};

class ResponseStreamBuffered final {
public:
    [[nodiscard]] constexpr bool failed() const noexcept {
        return failed_;
    }

    [[nodiscard]] HttpResponse takeResponse() && noexcept {
        return std::move(response_);
    }

private:
    friend class ResponseStreamDispatchResult;

    ResponseStreamBuffered(
        HttpResponse response,
        bool failed) noexcept
        : response_(std::move(response)),
          failed_(failed) {}

    HttpResponse response_;
    bool failed_;
};

// The primary alternatives follow the commit boundary because it determines
// which payload exists: committed outcomes own a status, buffered outcomes own
// a response, and a pre-commit peer abort owns neither.
class ResponseStreamDispatchResult final {
public:
    [[nodiscard]] static ResponseStreamDispatchResult makeCommitted(
        std::uint16_t status,
        ResponseStreamCommittedOutcome outcome) noexcept {
        return ResponseStreamDispatchResult(
            ResponseStreamCommitted(status, outcome));
    }

    [[nodiscard]] static ResponseStreamDispatchResult
    makePeerAbortedBeforeCommit() noexcept {
        return ResponseStreamDispatchResult(
            ResponseStreamPeerAbortedBeforeCommit{});
    }

    [[nodiscard]] static ResponseStreamDispatchResult makeBuffered(
        HttpResponse response,
        bool failed) noexcept {
        return ResponseStreamDispatchResult(
            ResponseStreamBuffered(std::move(response), failed));
    }

    [[nodiscard]] const ResponseStreamCommitted*
    committed() const & noexcept {
        return std::get_if<ResponseStreamCommitted>(&value_);
    }
    [[nodiscard]] const ResponseStreamCommitted*
    committed() const && = delete;

    [[nodiscard]] const ResponseStreamPeerAbortedBeforeCommit*
    peerAbortedBeforeCommit() const & noexcept {
        return std::get_if<ResponseStreamPeerAbortedBeforeCommit>(&value_);
    }
    [[nodiscard]] const ResponseStreamPeerAbortedBeforeCommit*
    peerAbortedBeforeCommit() const && = delete;

    [[nodiscard]] const ResponseStreamBuffered* buffered() const & noexcept {
        return std::get_if<ResponseStreamBuffered>(&value_);
    }
    [[nodiscard]] const ResponseStreamBuffered* buffered() const && = delete;

    [[nodiscard]] ResponseStreamBuffered* buffered() & noexcept {
        return std::get_if<ResponseStreamBuffered>(&value_);
    }
    [[nodiscard]] ResponseStreamBuffered* buffered() && = delete;

private:
    using Value = std::variant<
        ResponseStreamCommitted,
        ResponseStreamPeerAbortedBeforeCommit,
        ResponseStreamBuffered>;

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
            co_return ResponseStreamDispatchResult::makeCommitted(
                committedResponseStreamStatus(sink),
                ResponseStreamCommittedOutcome::kPeerAborted);
        }
        if (!result.has_value()) {
            if (!sink.committed()) {
                throw std::logic_error(
                    "handled response stream has no committed protocol plan");
            }
            co_return ResponseStreamDispatchResult::makeCommitted(
                committedResponseStreamStatus(sink),
                ResponseStreamCommittedOutcome::kCompleted);
        }
        if (sink.committed()) {
            throw std::logic_error(
                "buffered stream result followed a committed response head");
        }
        co_return ResponseStreamDispatchResult::makeBuffered(
            std::move(*result),
            false);
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
        co_return ResponseStreamDispatchResult::makeCommitted(
            committedResponseStreamStatus(sink),
            ResponseStreamCommittedOutcome::kPeerAborted);
    }
    if (sink.committed()) {
        co_return ResponseStreamDispatchResult::makeCommitted(
            committedResponseStreamStatus(sink),
            ResponseStreamCommittedOutcome::kFailed);
    }
    auto response = co_await routes.handleException(
        request, requestMemory, exception, services);
    co_return ResponseStreamDispatchResult::makeBuffered(
        std::move(response),
        true);
}

}  // namespace ruvia::detail
