#pragma once

// Streaming response sink for the sans-I/O HTTP/2 session (ruvia-web).
//
// Mirrors the coroutine Http2ResponseStreamSink but drives an Http2Connection through
// its submit* API instead of a socket: commit() emits the streaming HEADERS (no
// Content-Length) via submitStreamingResponseHead, write() streams DATA via submitData,
// and end() closes the stream. The shared dispatchResponseStreamWith machinery calls
// these via the responseStream*Thunk<Sink> function pointers, so the method set matches.
//
// Backpressure: a window-blocked submit parks on the stream's signal until the
// session's reader reports the core drained the remainder (takeUnblockedStreams), so a
// slow consumer stalls the producer instead of growing the out-buffer without bound.
// Trailers are HPACK-collected and emitted as the final HEADERS (END_STREAM) frame.

#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <asio/steady_timer.hpp>

#include "net/http2/Http2Connection.h"
#include "net/http2/Http2Hpack.h"
#include "net/http2/Http2SansIoWsTransport.h"
#include "net/server/HttpResponseStreamHead.h"
#include "net/server/HttpResponseStreamState.h"
#include "runtime/AsioAwait.h"
#include "detail/HttpAsciiCase.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/http/detail/PmrString.h"

namespace ruvia {
class Context;  // only forwarded as Context* via the type-erased bindContext thunk
}

namespace ruvia::detail {

template <typename Executor>
class Http2SansIoResponseStreamSink final {
public:
    Http2SansIoResponseStreamSink(
        Http2Connection& connection,
        std::uint32_t streamId,
        ResponseBodyMode mode,
        std::pmr::memory_resource* resource,
        Executor executor,
        asio::steady_timer* writeSignal = nullptr,
        Http2SansIoStreamSignal* streamSignal = nullptr) noexcept
        : connection_(connection),
          streamId_(streamId),
          mode_(mode),
          scratch_(resource),
          trailers_(resource),
          lowerName_(resource),
          executor_(executor),
          writeSignal_(writeSignal),
          streamSignal_(streamSignal) {}

    [[nodiscard]] bool committed() const noexcept { return state_.committed(); }

    [[nodiscard]] bool aborted() const noexcept {
        auto* stream = connection_.stream(streamId_);
        return stream == nullptr || stream->isReset();
    }

    void bindContext(Context* context, ResponseStreamState::StreamingHeadThunk streamingHead) noexcept {
        state_.bindContext(context, streamingHead);
    }

    [[nodiscard]] std::pmr::string& scratch() noexcept {
        clearPmrStringRetainingSmall(scratch_);
        return scratch_;
    }

    Task<void> write(std::string_view chunk) {
        if (chunk.empty()) {
            co_return;
        }
        co_await commit();
        state_.ensureBodyAllowed();
        const auto result = connection_.submitData(streamId_, chunk, /*endStream=*/false);
        wakeWriter();
        if (result == Http2SubmitResult::kBlocked) {
            co_await awaitSendWindow();
        }
    }

    Task<void> sleep(std::chrono::milliseconds duration) {
        asio::steady_timer timer(executor_, duration);
        const auto ec = co_await asyncError([&timer](auto handler) mutable {
            timer.async_wait(std::move(handler));
        });
        if (ec) {
            throw std::system_error(ec);
        }
    }

    // RFC 9113 §8.1 trailers are queued before the stream ends and HPACK encoded into
    // a header block flushed at end() as a trailing HEADERS frame carrying END_STREAM,
    // in place of the empty END_STREAM DATA frame. Mirrors the retired coroutine sink.
    void addTrailer(std::string_view name, std::string_view value) {
        state_.ensureTrailerAllowed(name, value);
        lowerName_.clear();
        lowerName_.reserve(name.size());
        for (const char ch : name) {
            lowerName_.push_back(static_cast<char>(httpAsciiToLower(static_cast<unsigned char>(ch))));
        }
        HpackEncoder::encodeHeader(trailers_, lowerName_, value);
    }

    Task<void> end() {
        if (state_.ended()) {
            co_return;
        }
        co_await commit();
        if (state_.bodyForbidden()) {
            state_.markEnded();
            co_return;
        }
        if (trailers_.empty()) {
            (void)connection_.submitData(streamId_, {}, /*endStream=*/true);
        } else {
            connection_.submitTrailers(
                streamId_, std::string_view(trailers_.data(), trailers_.size()));
        }
        wakeWriter();
        state_.markEnded();
    }

private:
    Task<void> commit() {
        if (state_.committed()) {
            co_return;
        }
        auto streamHead = prepareResponseStreamHead(
            state_.streamingHead(), mode_, ResponseStreamFraming::kHttp2DataFrames);
        state_.markCommitted(streamHead.bodyForbidden());
        connection_.submitStreamingResponseHead(
            streamId_, streamHead.response(), streamHead.bodyForbidden());
        wakeWriter();
        if (state_.bodyForbidden()) {
            state_.markEnded();
        }
    }

    // Wake the session's single writer so submitted bytes actually flush; without
    // this, output produced between inbound frames sits in the core's buffer until
    // the peer happens to send something (SSE over a quiet connection stalls).
    void wakeWriter() noexcept {
        if (writeSignal_ != nullptr) {
            writeSignal_->cancel();
        }
    }

    // Park until the reader reports the window-blocked remainder drained. A spurious
    // wake just re-checks; if the stream dies or the session tears down, stop waiting
    // (the dispatch wrapper observes the abort via peerAborted / isReset).
    Task<void> awaitSendWindow() {
        while (streamSignal_ != nullptr && !streamSignal_->ended &&
               connection_.hasBlockedSend(streamId_)) {
            auto* stream = connection_.stream(streamId_);
            if (stream == nullptr || stream->isReset()) {
                co_return;
            }
            co_await streamSignal_->wait();
        }
    }

    Http2Connection& connection_;
    std::uint32_t streamId_;
    ResponseBodyMode mode_;
    ResponseStreamState state_;
    std::pmr::string scratch_;
    std::pmr::string trailers_;   // HPACK-encoded trailer block, flushed at end()
    std::pmr::string lowerName_;
    Executor executor_;
    asio::steady_timer* writeSignal_{nullptr};
    Http2SansIoStreamSignal* streamSignal_{nullptr};
};

}  // namespace ruvia::detail
