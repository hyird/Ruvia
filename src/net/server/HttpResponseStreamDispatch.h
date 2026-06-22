#pragma once

#include <exception>
#include <utility>

#include "../../router/RouteTable.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/http/Streaming.h"

namespace ruvia::detail {

// How a response-stream dispatch finished, independent of transport. The
// HTTP/1.1 and HTTP/2 sinks differ only in concrete type — both expose the same
// static thunk set and committed() — so the dispatch + commit/exception
// decision tree lives here once and each transport maps the outcome onto its own
// session control flow.
enum class ResponseStreamDispatchOutcome {
    kStreamed,            // headers/body committed and the stream was ended
    kAbortedByPeer,       // the peer reset the stream mid-dispatch (HTTP/2)
    kAbortedAfterCommit,  // the handler threw after bytes were already committed
    kBuffered,            // the handler returned a buffered response instead of streaming
    kFailedBeforeCommit,  // the handler/routing threw before commit; response holds the error
};

struct ResponseStreamDispatchResult final {
    ResponseStreamDispatchOutcome outcome{};
    HttpResponse response;
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
    RouteServices services,
    bool closeConnectionOnError,
    PeerAborted peerAborted) {
    using Outcome = ResponseStreamDispatchOutcome;
    ResponseStreamWriter responseStream(
        &sink,
        &Sink::writeThunk,
        &Sink::endThunk,
        &Sink::bindContextThunk,
        &Sink::scratchThunk,
        &Sink::addTrailerThunk);

    std::exception_ptr exception;
    HttpResponse response(requestMemory.resource());
    bool streamHandled = false;
    try {
        auto result = co_await routes.dispatchResponseStream(
            request, resolution, requestMemory, responseStream, services);
        streamHandled = result.streamHandled;
        if (peerAborted()) {
            co_return ResponseStreamDispatchResult{Outcome::kAbortedByPeer, std::move(response)};
        }
        if (streamHandled || sink.committed()) {
            co_await responseStream.end();
            co_return ResponseStreamDispatchResult{Outcome::kStreamed, std::move(response)};
        }
        response = std::move(result.response);
    } catch (...) {
        exception = std::current_exception();
    }

    if (exception != nullptr) {
        if (sink.committed()) {
            co_return ResponseStreamDispatchResult{Outcome::kAbortedAfterCommit, std::move(response)};
        }
        response = co_await routes.handleException(
            request, requestMemory, exception, closeConnectionOnError, services);
        co_return ResponseStreamDispatchResult{Outcome::kFailedBeforeCommit, std::move(response)};
    }
    co_return ResponseStreamDispatchResult{Outcome::kBuffered, std::move(response)};
}

}  // namespace ruvia::detail
