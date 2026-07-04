#pragma once

#include <chrono>
#include <exception>
#include <utility>

#include "../../router/RouteTable.h"
#include "../../http/StreamingInternal.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/http/Streaming.h"

namespace ruvia::detail {

template <typename Sink>
Task<void> responseStreamWriteThunk(void* target, std::string_view chunk) {
    co_await static_cast<Sink*>(target)->write(chunk);
}

template <typename Sink>
Task<void> responseStreamEndThunk(void* target) {
    co_await static_cast<Sink*>(target)->end();
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
void responseStreamAddTrailerThunk(void* target, std::string_view name, std::string_view value) {
    static_cast<Sink*>(target)->addTrailer(name, value);
}

template <typename Sink>
void responseStreamBindContextThunk(void* target, Context* context) noexcept {
    static_cast<Sink*>(target)->bindContext(context);
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
        &responseStreamAddTrailerThunk<Sink>,
        &responseStreamCommittedThunk<Sink>,
        &responseStreamAbortedThunk<Sink>);
}

class ResponseStreamDispatchResult final {
public:
    [[nodiscard]] static ResponseStreamDispatchResult streamed(HttpResponse response) {
        return ResponseStreamDispatchResult(Outcome::kStreamed, std::move(response));
    }

    [[nodiscard]] static ResponseStreamDispatchResult abortedByPeer(HttpResponse response) {
        return ResponseStreamDispatchResult(Outcome::kAbortedByPeer, std::move(response));
    }

    [[nodiscard]] static ResponseStreamDispatchResult abortedAfterCommit(HttpResponse response) {
        return ResponseStreamDispatchResult(Outcome::kAbortedAfterCommit, std::move(response));
    }

    [[nodiscard]] static ResponseStreamDispatchResult buffered(HttpResponse response) {
        return ResponseStreamDispatchResult(Outcome::kBuffered, std::move(response));
    }

    [[nodiscard]] static ResponseStreamDispatchResult failedBeforeCommit(HttpResponse response) {
        return ResponseStreamDispatchResult(Outcome::kFailedBeforeCommit, std::move(response));
    }

    [[nodiscard]] bool streamed() const noexcept {
        return outcome_ == Outcome::kStreamed;
    }

    [[nodiscard]] bool abortedByPeer() const noexcept {
        return outcome_ == Outcome::kAbortedByPeer;
    }

    [[nodiscard]] bool abortedAfterCommit() const noexcept {
        return outcome_ == Outcome::kAbortedAfterCommit;
    }

    [[nodiscard]] bool aborted() const noexcept {
        return abortedByPeer() || abortedAfterCommit();
    }

    [[nodiscard]] bool buffered() const noexcept {
        return outcome_ == Outcome::kBuffered;
    }

    [[nodiscard]] bool failedBeforeCommit() const noexcept {
        return outcome_ == Outcome::kFailedBeforeCommit;
    }

    [[nodiscard]] bool hasBufferedResponse() const noexcept {
        return buffered() || failedBeforeCommit();
    }

    [[nodiscard]] bool sessionFinished() const noexcept {
        return streamed() || aborted();
    }

    [[nodiscard]] HttpResponse takeResponse() noexcept {
        return std::move(response_);
    }

private:
    // How a response-stream dispatch finished, independent of transport.
    // HTTP/1.1 and HTTP/2 sinks differ only in concrete type; callers consume
    // semantic predicates instead of switching on these internal states.
    enum class Outcome {
        kStreamed,
        kAbortedByPeer,
        kAbortedAfterCommit,
        kBuffered,
        kFailedBeforeCommit,
    };

    ResponseStreamDispatchResult(Outcome outcome, HttpResponse response)
        : outcome_(outcome),
          response_(std::move(response)) {}

    Outcome outcome_{Outcome::kBuffered};
    HttpResponse response_;
};

// Drives a response-stream route over an already-constructed sink. peerAborted
// is a predicate consulted after the handler returns so a transport that can be
// reset out from under the handler (HTTP/2, where the reset flag is a bit-field)
// can report kAbortedByPeer; HTTP/1.1 passes a constant-false predicate, which
// folds away. closeConnectionOnError governs the error response produced when
// the handler throws before committing any bytes.
template <typename Sink, typename PeerAborted>
Task<ResponseStreamDispatchResult> dispatchResponseStreamWith(
    Sink& sink,
    const RouteTable& routes,
    const HttpRequest& request,
    const RouteResolution& resolution,
    RequestMemory& requestMemory,
    ContextServices services,
    bool closeConnectionOnError,
    PeerAborted peerAborted) {
    auto responseStream = makeResponseStreamWriter(sink);

    std::exception_ptr exception;
    HttpResponse response(requestMemory.resource());
    try {
        auto result = co_await routes.dispatchResponseStream(
            request, resolution, requestMemory, responseStream, services);
        if (peerAborted()) {
            co_return ResponseStreamDispatchResult::abortedByPeer(std::move(response));
        }
        if (result.streamHandled() || sink.committed()) {
            co_await responseStream.end();
            co_return ResponseStreamDispatchResult::streamed(std::move(response));
        }
        response = result.takeResponse();
    } catch (...) {
        exception = std::current_exception();
    }

    if (exception != nullptr) {
        if (sink.committed()) {
            co_return ResponseStreamDispatchResult::abortedAfterCommit(std::move(response));
        }
        response = co_await routes.handleException(
            request, requestMemory, exception, closeConnectionOnError, services);
        co_return ResponseStreamDispatchResult::failedBeforeCommit(std::move(response));
    }
    co_return ResponseStreamDispatchResult::buffered(std::move(response));
}

}  // namespace ruvia::detail
