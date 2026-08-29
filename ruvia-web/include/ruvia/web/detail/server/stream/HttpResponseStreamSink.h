#pragma once

#include "ruvia/http/HttpHeader.h"

#include "ruvia/core/detail/io/AsioAwait.h"

#include "ruvia/http/detail/server/HttpResponseHead.h"
#include "ruvia/http/detail/server/HttpResponseStreamHead.h"
#include "ruvia/http/detail/http1/Http1ChunkedFraming.h"
#include "ruvia/http/detail/http1/Http1ServerSemantics.h"
#include "ruvia/web/detail/server/stream/HttpResponseStreamState.h"
#include "ruvia/web/detail/server/response/HttpServerResponseState.h"
#include "ruvia/web/detail/server/response/HttpStreamingResponseCompression.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/Timer.h"
#include "ruvia/web/Context.h"
#include "ruvia/http/detail/util/PmrString.h"
#include "ruvia/core/memory/MemoryPool.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio.hpp>

namespace ruvia::detail {

template <typename Stream, typename ScannerEntry>
class ResponseStreamSink final {
public:
    ResponseStreamSink(Stream& stream, WorkerMemory& memory, ResponseHeadBuffer& head,
        ScannerEntry& scannerEntry, const WorkerHandle& worker, ResponseStreamKind kind,
        Http1ResponseStreamPlan plan, HttpResponseCodingSelection responseCoding,
        HttpResponseCodingAvailability responseCodingAvailability) noexcept
        : stream_(stream),
          head_(head),
          trailers_(memory.resource()),
          scannerEntry_(scannerEntry),
          worker_(worker),
          kind_(kind),
          plan_(plan),
          connectionPlan_(plan.requestConnectionPlan().requireClose()),
          compression_(memory.resource(), responseCoding, responseCodingAvailability) {}

    ResponseStreamSink(Stream&, WorkerMemory&, ResponseHeadBuffer&, ScannerEntry&, WorkerHandle&&,
        ResponseStreamKind, Http1ResponseStreamPlan, HttpResponseCodingSelection,
        HttpResponseCodingAvailability) = delete;

    [[nodiscard]] bool committed() const noexcept {
        return state_.committed();
    }

    [[nodiscard]] const ResponseStreamCommitPlan* commitPlan() const& noexcept {
        return state_.commitPlan();
    }
    const ResponseStreamCommitPlan* commitPlan() const&& = delete;

    [[nodiscard]] bool aborted() const noexcept {
        return state_.aborted();
    }

    [[nodiscard]] Http1ServerConnectionPlan connectionPlan() const noexcept {
        return connectionPlan_;
    }

    template <typename Sink>
    friend Task<void> responseStreamWriteThunk(void*, std::string_view);
    template <typename Sink>
    friend Task<void> responseStreamEndThunk(void*, std::span<const HttpHeaderView>);
    template <typename Sink>
    friend Task<TimerSleepResult> responseStreamSleepThunk(
        void*, std::chrono::milliseconds, const StopToken&);
    template <typename Sink>
    friend void responseStreamBindContextThunk(
        void*, Context*, ResponseStreamState::StreamingHeadThunk);
    template <typename Sink>
    friend void responseStreamReleaseContextThunk(void*) noexcept;

private:
    void bindContext(Context* context, ResponseStreamState::StreamingHeadThunk streamingHead) {
        state_.bindContext(context, streamingHead);
    }

    void releaseContext() noexcept {
        state_.releaseContext();
    }

    Task<void> commit(ResponseTrailerIntent trailerIntent) {
        if (state_.committed()) {
            if (trailerIntent == ResponseTrailerIntent::kPresent) {
                state_.ensureTrailersAllowed(ResponseStreamTrailerFraming::kHttp1Chunked);
            }
            co_return;
        }

        try {
            auto response = state_.streamingHead();
            compression_.prepare(plan_.requestMethod(), response, kind_);
            auto prepareResult =
                prepareHttp1ResponseStreamHead(std::move(response), kind_, plan_, trailerIntent);
            if (const auto* failure = prepareResult.failure()) {
                throw failure->exception();
            }
            auto* prepared = prepareResult.prepared();
            if (prepared == nullptr) {
                throw std::logic_error(
                    "HTTP/1 stream preparation returned no terminal alternative");
            }
            auto streamHead = std::move(*prepared);
            if (trailerIntent == ResponseTrailerIntent::kPresent &&
                streamHead.commitPlan().trailerFraming() !=
                    ResponseStreamTrailerFraming::kHttp1Chunked) {
                throw std::logic_error("response framing does not support trailers");
            }
            compression_.activate(streamHead.commitPlan().bodyPlan());

            head_.reset();
            appendResponseHead(streamHead.response(), head_, streamHead.responseHeadPlan());
            connectionPlan_ = streamHead.connectionPlan();
            // Mark committed before the write; a partial header flush must never be
            // followed by the normal error-response path on the same socket.
            state_.markCommitted(streamHead.commitPlan());
        } catch (...) {
            // Representation metadata and protocol framing are one pre-wire
            // transaction. Once either side has failed, no handler retry may
            // manufacture a second head from half-prepared compression state.
            if (!state_.committed()) {
                compression_.abort();
                state_.markAborted();
            }
            throw;
        }
        const auto writeCompletion =
            co_await asyncAsio([this, headView = head_.view()](auto handler) mutable {
                asio::async_write(stream_, asio::buffer(headView), std::move(handler));
            });
        const auto ec = writeCompletion.errorCode();
        if (ec) {
            state_.markAborted();
            throw std::system_error(ec);
        }
        scannerEntry_.touch();
    }

    Task<TimerSleepResult> sleep(std::chrono::milliseconds duration, const StopToken& stopToken) {
        const auto result = co_await sleepFor(worker_, duration, stopToken);
        if (result == TimerSleepResult::kElapsed) {
            scannerEntry_.touch();
        }
        co_return result;
    }

    Task<void> write(std::string_view chunk) {
        if (chunk.empty()) {
            co_return;
        }
        co_await commit(ResponseTrailerIntent::kNone);
        if (state_.bodySuppressedComplete()) {
            // ensureBodyAllowed() is about to throw ResponseStreamHeadOnlyComplete
            // synchronously (commit() returned without suspending on an already
            // body-suppressed head). Suspend once first: a handler that catches
            // the control signal and keeps writing (an SSE loop answering a HEAD
            // or 304) then yields the worker thread each pass instead of
            // hard-spinning the event loop with no suspension point. The minimal
            // positive delay is required because a zero duration is await_ready.
            static_cast<void>(co_await sleepFor(worker_, std::chrono::steady_clock::duration(1)));
        }
        state_.ensureBodyAllowed();

        if (compression_.active()) {
            if (compression_.write(chunk) == HttpContentEncodeStep::kFailure) {
                state_.markAborted();
                throw std::runtime_error("HTTP response stream content encoding failed");
            }
            if (compression_.output().empty()) {
                co_return;
            }
            co_await writeEncoded(compression_.output());
            co_return;
        }

        co_await writeEncoded(chunk);
    }

    Task<void> writeEncoded(std::string_view chunk) {
        if (chunk.empty()) {
            co_return;
        }

        if (plan_.framing() == ResponseStreamFraming::kHttp1CloseDelimited) {
            // No chunk framing: write the raw body bytes. The connection close
            // (forced once the stream ends) is what delimits the message.
            const auto writeCompletion = co_await asyncAsio([this, chunk](auto handler) mutable {
                asio::async_write(stream_, asio::buffer(chunk), std::move(handler));
            });
            const auto rawEc = writeCompletion.errorCode();
            if (rawEc) {
                state_.markAborted();
                throw std::system_error(rawEc);
            }
            scannerEntry_.touch();
            co_return;
        }

        const Http1ChunkHeader chunkHeader(chunk.size());
        const std::array<asio::const_buffer, 3> buffers{asio::buffer(chunkHeader.view()),
            asio::buffer(chunk), asio::buffer(kHttp1ChunkDataTerminator)};
        const auto writeCompletion = co_await asyncAsio([this, &buffers](auto handler) mutable {
            asio::async_write(stream_, buffers, std::move(handler));
        });
        const auto writeEc = writeCompletion.errorCode();
        if (writeEc) {
            state_.markAborted();
            throw std::system_error(writeEc);
        }
        scannerEntry_.touch();
    }

    Task<void> end(std::span<const HttpHeaderView> trailers) {
        if (state_.ended()) {
            if (!trailers.empty()) {
                throw std::logic_error("response stream is already ended");
            }
            co_return;
        }

        const auto trailerResult = validatedResponseTrailerSection(trailers);
        const auto& trailerSection = *trailerResult.section();
        const auto trailerIntent = responseTrailerIntent(trailerSection);
        if (!trailerSection.empty()) {
            clearPmrStringRetainingSmall(trailers_);
            appendHttp1TrailerSection(trailers_, trailerSection);
        } else {
            clearPmrStringRetainingSmall(trailers_);
        }

        co_await commit(trailerIntent);
        if (state_.ended()) {
            co_return;
        }
        if (compression_.active()) {
            if (compression_.finish() != HttpContentEncodeStep::kFinished) {
                state_.markAborted();
                throw std::runtime_error(
                    "HTTP response stream content encoding finalization failed");
            }
            co_await writeEncoded(compression_.output());
        }
        if (plan_.framing() == ResponseStreamFraming::kHttp1CloseDelimited) {
            // No last-chunk terminator: the connection close delimits the body.
            state_.markEnded();
            co_return;
        }

        // The protocol primitive owns the last-chunk and trailer-section delimiters;
        // this runtime layer only submits their byte views to the socket.
        const std::array<asio::const_buffer, 3> buffers{asio::buffer(kHttp1LastChunkPrefix),
            asio::buffer(trailers_), asio::buffer(kHttp1TrailerSectionTerminator)};
        const auto writeCompletion = co_await asyncAsio([this, &buffers](auto handler) mutable {
            asio::async_write(stream_, buffers, std::move(handler));
        });
        const auto ec = writeCompletion.errorCode();
        if (ec) {
            state_.markAborted();
            throw std::system_error(ec);
        }
        state_.markEnded();
        scannerEntry_.touch();
    }

    Stream& stream_;
    ResponseHeadBuffer& head_;
    std::pmr::string trailers_;
    ScannerEntry& scannerEntry_;
    // The connection/server owns an address-stable handle for the complete
    // route dispatch. Streaming must not acquire shared ownership per request.
    const WorkerHandle& worker_;
    ResponseStreamKind kind_;
    Http1ResponseStreamPlan plan_;
    Http1ServerConnectionPlan connectionPlan_;
    HttpStreamingResponseCompression compression_;
    ResponseStreamState state_;
};

}  // namespace ruvia::detail
