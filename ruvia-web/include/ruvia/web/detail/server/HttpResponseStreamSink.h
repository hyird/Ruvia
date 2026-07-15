#pragma once

#include "ruvia/http/HttpHeader.h"

#include "ruvia/core/detail/AsioAwait.h"

#include "ruvia/http/detail/server/HttpResponseHead.h"
#include "ruvia/http/detail/server/HttpResponseStreamHead.h"
#include "ruvia/http/detail/http1/Http1ChunkedFraming.h"
#include "ruvia/http/detail/http1/Http1ServerSemantics.h"
#include "ruvia/web/detail/server/HttpResponseStreamState.h"
#include "ruvia/web/detail/server/HttpServerResponseState.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/Timer.h"
#include "ruvia/web/Context.h"
#include "ruvia/http/detail/PmrString.h"
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
    ResponseStreamSink(
        Stream& stream,
        WorkerMemory& memory,
        ResponseHeadBuffer& head,
        ScannerEntry& scannerEntry,
        WorkerHandle worker,
        ResponseStreamKind kind,
        Http1ResponseStreamPlan plan) noexcept
        : stream_(stream),
          head_(head),
          scratch_(memory.resource()),
          trailers_(memory.resource()),
          scannerEntry_(scannerEntry),
          worker_(std::move(worker)),
          kind_(kind),
          plan_(plan),
          connectionPlan_(plan.requestConnectionPlan().requireClose()) {}

    [[nodiscard]] bool committed() const noexcept { return state_.committed(); }

    [[nodiscard]] const ResponseStreamCommitPlan*
    commitPlan() const & noexcept {
        return state_.commitPlan();
    }
    const ResponseStreamCommitPlan* commitPlan() const && = delete;

    [[nodiscard]] bool aborted() const noexcept { return state_.aborted(); }

    [[nodiscard]] Http1ServerConnectionPlan connectionPlan() const noexcept {
        return connectionPlan_;
    }

    template <typename Sink>
    friend Task<void> responseStreamWriteThunk(void*, std::string_view);
    template <typename Sink>
    friend Task<void> responseStreamEndThunk(void*, std::span<const HttpHeaderView>);
    template <typename Sink>
    friend Task<void> responseStreamSleepThunk(void*, std::chrono::milliseconds);
    template <typename Sink>
    friend void responseStreamBindContextThunk(
        void*, Context*, ResponseStreamState::StreamingHeadThunk);
    template <typename Sink>
    friend void responseStreamReleaseContextThunk(void*) noexcept;
    template <typename Sink>
    friend std::pmr::string& responseStreamScratchThunk(void*) noexcept;

private:
    void bindContext(
        Context* context,
        ResponseStreamState::StreamingHeadThunk streamingHead) {
        state_.bindContext(context, streamingHead);
    }

    void releaseContext() noexcept { state_.releaseContext(); }

    [[nodiscard]] std::pmr::string& scratch() noexcept {
        clearPmrStringRetainingSmall(scratch_);
        return scratch_;
    }

    Task<void> commit(ResponseTrailerIntent trailerIntent) {
        if (state_.committed()) {
            if (trailerIntent == ResponseTrailerIntent::kPresent) {
                state_.ensureTrailersAllowed(
                    ResponseStreamTrailerFraming::kHttp1Chunked);
            }
            co_return;
        }

        auto prepareResult = prepareHttp1ResponseStreamHead(
            state_.streamingHead(),
            kind_,
            plan_,
            trailerIntent);
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
        connectionPlan_ = streamHead.connectionPlan();

        head_.reset();
        appendResponseHead(
            streamHead.response(),
            head_,
            streamHead.responseHeadPlan());
        // Mark committed before the write; a partial header flush must never be
        // followed by the normal error-response path on the same socket.
        state_.markCommitted(streamHead.commitPlan());
        const auto writeCompletion = co_await asyncAsio(
            [this, headView = head_.view()](auto handler) mutable {
                asio::async_write(
                    stream_, asio::buffer(headView), std::move(handler));
            });
        const auto ec = writeCompletion.errorCode();
        if (ec) {
            state_.markAborted();
            throw std::system_error(ec);
        }
        scannerEntry_.touch();
    }

    Task<void> sleep(std::chrono::milliseconds duration) {
        co_await sleepFor(worker_, duration);
        scannerEntry_.touch();
    }

    Task<void> write(std::string_view chunk) {
        if (chunk.empty()) {
            co_return;
        }
        co_await commit(ResponseTrailerIntent::kNone);
        state_.ensureBodyAllowed();

        if (plan_.framing() == ResponseStreamFraming::kHttp1CloseDelimited) {
            // No chunk framing: write the raw body bytes. The connection close
            // (forced once the stream ends) is what delimits the message.
            const auto writeCompletion = co_await asyncAsio(
                [this, chunk](auto handler) mutable {
                    asio::async_write(
                        stream_, asio::buffer(chunk), std::move(handler));
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
        const std::array<asio::const_buffer, 3> buffers{
            asio::buffer(chunkHeader.view()),
            asio::buffer(chunk),
            asio::buffer(kHttp1ChunkDataTerminator)};
        const auto writeCompletion = co_await asyncAsio(
            [this, &buffers](auto handler) mutable {
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

        const auto trailerResult = httpResponseTrailerSection(trailers);
        if (const auto* failure = trailerResult.failure()) {
            throw failure->exception();
        }
        const auto& trailerSection = *trailerResult.section();
        const auto trailerIntent = trailerSection.empty()
            ? ResponseTrailerIntent::kNone
            : ResponseTrailerIntent::kPresent;
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
        if (plan_.framing() == ResponseStreamFraming::kHttp1CloseDelimited) {
            // No last-chunk terminator: the connection close delimits the body.
            state_.markEnded();
            co_return;
        }

        // The protocol primitive owns the last-chunk and trailer-section delimiters;
        // this runtime layer only submits their byte views to the socket.
        const std::array<asio::const_buffer, 3> buffers{
            asio::buffer(kHttp1LastChunkPrefix),
            asio::buffer(trailers_),
            asio::buffer(kHttp1TrailerSectionTerminator)};
        const auto writeCompletion = co_await asyncAsio(
            [this, &buffers](auto handler) mutable {
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
    std::pmr::string scratch_;
    std::pmr::string trailers_;
    ScannerEntry& scannerEntry_;
    WorkerHandle worker_;
    ResponseStreamKind kind_;
    Http1ResponseStreamPlan plan_;
    Http1ServerConnectionPlan connectionPlan_;
    ResponseStreamState state_;
};

}  // namespace ruvia::detail
