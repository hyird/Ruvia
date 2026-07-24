#pragma once

#include "ruvia/http/HttpHeader.h"

// Streaming response sink for the sans-I/O HTTP/2 session (ruvia-web).
//
// Mirrors the coroutine Http2ResponseStreamSink but drives an Http2Connection through
// its submit* API instead of a socket: commit() emits the streaming HEADERS (no
// Content-Length) via submitStreamingResponseHead, write() streams DATA via submitData,
// and end() closes the stream. The shared dispatchResponseStreamWith machinery calls
// these via the responseStream*Thunk<Sink> function pointers, so the method set matches.
//
// Backpressure: a window-blocked submit parks on the stream's signal until the
// session's reader reports the core drained the remainder (takeDrainedDataStreams), so
// a slow consumer stalls the producer instead of growing the out-buffer without bound.
// Trailers are submitted semantically to the HTTP core, which owns their protocol bytes.

#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "ruvia/http/detail/http2/Http2Connection.h"
#include "ruvia/http/detail/server/HttpResponseTrailers.h"
#include "ruvia/web/detail/http2/Http2SansIoSendWindow.h"
#include "ruvia/web/detail/http2/Http2SansIoStreamRuntime.h"
#include "ruvia/http/detail/server/HttpResponseStreamHead.h"
#include "ruvia/web/detail/server/stream/HttpResponseStreamState.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/detail/worker/WorkerSignal.h"
#include "ruvia/http/detail/util/PmrString.h"

namespace ruvia {
class Context;  // only forwarded as Context* via the type-erased bindContext thunk
}

namespace ruvia::detail {

class Http2SansIoResponseStreamSink final {
public:
    Http2SansIoResponseStreamSink(Http2Connection& connection, std::uint32_t streamId, ResponseStreamKind kind, const WorkerHandle& worker, WorkerSignal& writeSignal, Http2SansIoStreamSignal& streamSignal) noexcept
        : connection_(connection),
          streamId_(streamId),
          kind_(kind),
          worker_(&worker),
          writeSignal_(writeSignal),
          streamSignal_(streamSignal) {}

    Http2SansIoResponseStreamSink(Http2Connection&, std::uint32_t, ResponseStreamKind, WorkerHandle&&, WorkerSignal&, Http2SansIoStreamSignal&) = delete;

    [[nodiscard]] bool committed() const noexcept {
        return state_.committed();
    }

    [[nodiscard]] const ResponseStreamCommitPlan* commitPlan() const& noexcept {
        return state_.commitPlan();
    }
    const ResponseStreamCommitPlan* commitPlan() const&& = delete;

    [[nodiscard]] bool aborted() const noexcept {
        auto* stream = connection_.stream(streamId_);
        return stream == nullptr || stream->isAborted() || streamSignal_.terminated();
    }

    void bindContext(Context* context, ResponseStreamState::StreamingHeadThunk streamingHead) {
        state_.bindContext(context, streamingHead);
    }

    void releaseContext() noexcept {
        state_.releaseContext();
    }

    Task<void> write(std::string_view chunk) {
        throwIfTerminated();
        if (chunk.empty()) {
            co_return;
        }
        co_await commit(ResponseTrailerIntent::kNone);
        if (state_.bodySuppressedComplete()) {
            // Same head-only guard as the HTTP/1 sink: suspend once before the
            // synchronous ResponseStreamHeadOnlyComplete throw so a handler that
            // catches it and keeps writing yields the worker thread each pass
            // rather than hard-spinning the event loop. A zero duration is
            // await_ready, so the minimal positive tick is what forces the
            // suspension (termination short-circuits it back to ready).
            co_await Http2SansIoSleepAwaiter(*worker_, streamSignal_.termination(), std::chrono::steady_clock::duration(1));
        }
        state_.ensureBodyAllowed();
        for (;;) {
            const auto result = connection_.submitData(streamId_, chunk, Http2EndStream::kKeepOpen);
            wakeWriter();
            if (result == Http2DataSubmitStatus::kAccepted) {
                co_return;
            }
            if (result == Http2DataSubmitStatus::kClosed) {
                throw std::system_error(std::make_error_code(std::errc::connection_reset));
            }
            if (result == Http2DataSubmitStatus::kInvalidState) {
                throw std::logic_error("invalid HTTP/2 response stream DATA state");
            }
            if (result == Http2DataSubmitStatus::kContentLengthExceeded) {
                throw std::length_error("HTTP/2 response exceeds Content-Length");
            }
            if (result == Http2DataSubmitStatus::kContentLengthIncomplete) {
                throw std::length_error("HTTP/2 response ended before Content-Length");
            }
            const auto waitResult = co_await awaitHttp2SendWindow(connection_, streamId_, &streamSignal_);
            if (waitResult.aborted() != nullptr) {
                throw std::system_error(streamSignal_.terminated() ? streamSignal_.terminalError() : std::make_error_code(std::errc::connection_reset));
            }
            if (result == Http2DataSubmitStatus::kQueued) {
                co_return;  // the core already owned and drained this input
            }
            // kBackpressured accepted nothing; retry this same stable chunk view.
        }
    }

    Task<void> sleep(std::chrono::milliseconds duration) {
        co_await Http2SansIoSleepAwaiter(*worker_, streamSignal_.termination(), duration);
    }

    Task<void> end(std::span<const HttpHeaderView> trailers) {
        throwIfTerminated();
        if (state_.ended()) {
            if (!trailers.empty()) {
                throw std::logic_error("response stream is already ended");
            }
            co_return;
        }

        const auto trailerResult = validatedResponseTrailerSection(trailers);
        const auto& trailerSection = *trailerResult.section();
        const auto trailerIntent = responseTrailerIntent(trailerSection);
        // Preflight through the HTTP-owned result before committing the initial
        // response head. The typed section carries that proof to finishResponse.
        co_await commit(trailerIntent);
        if (state_.ended()) {
            co_return;
        }
        if (!trailerSection.empty()) {
            state_.ensureTrailersAllowed(ResponseStreamTrailerFraming::kHttp2TrailingHeaders);
        }
        const auto result = connection_.finishResponse(streamId_, trailerSection);
        wakeWriter();
        if (result == Http2FinishSubmitStatus::kClosed) {
            throw std::system_error(std::make_error_code(std::errc::connection_reset));
        }
        if (result == Http2FinishSubmitStatus::kInvalidState) {
            throw std::logic_error("invalid HTTP/2 response stream finish state");
        }
        if (result == Http2FinishSubmitStatus::kContentLengthIncomplete) {
            throw std::length_error("HTTP/2 response ended before Content-Length");
        }
        if (result == Http2FinishSubmitStatus::kQueued) {
            const auto waitResult = co_await awaitHttp2SendWindow(connection_, streamId_, &streamSignal_);
            if (waitResult.aborted() != nullptr) {
                throw std::system_error(streamSignal_.terminated() ? streamSignal_.terminalError() : std::make_error_code(std::errc::connection_reset));
            }
        }
        state_.markEnded();
    }

private:
    Task<void> commit(ResponseTrailerIntent trailerIntent) {
        throwIfTerminated();
        if (state_.committed()) {
            if (trailerIntent == ResponseTrailerIntent::kPresent) {
                state_.ensureTrailersAllowed(ResponseStreamTrailerFraming::kHttp2TrailingHeaders);
            }
            co_return;
        }
        const auto headResult = connection_.submitStreamingResponseHead(streamId_, state_.streamingHead(), kind_, trailerIntent);
        const auto* submittedHead = headResult.submitted();
        if (submittedHead == nullptr) {
            if (headResult.failure()->peerClosed()) {
                throw std::system_error(std::make_error_code(std::errc::connection_reset));
            }
            throw headResult.failure()->exception();
        }
        state_.markCommitted(*submittedHead);
        wakeWriter();
    }

    // Wake the session's single writer so submitted bytes actually flush; without
    // this, output produced between inbound frames sits in the core's buffer until
    // the peer happens to send something (SSE over a quiet connection stalls).
    void wakeWriter() noexcept {
        writeSignal_.notify();
    }

    void throwIfTerminated() const {
        if (streamSignal_.terminated()) {
            throw std::system_error(streamSignal_.terminalError());
        }
    }

    Http2Connection& connection_;
    std::uint32_t streamId_;
    ResponseStreamKind kind_;
    ResponseStreamState state_;
    // Borrow the stable server-owned handle. Holding a value here would add a
    // shared ownership operation for every streaming HTTP/2 request.
    const WorkerHandle* worker_;
    WorkerSignal& writeSignal_;
    Http2SansIoStreamSignal& streamSignal_;
};

}  // namespace ruvia::detail
