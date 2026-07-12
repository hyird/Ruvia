#pragma once

#include "ruvia/http/HttpHeader.h"

#include "ruvia/core/detail/AsioAwait.h"

#include "ruvia/http/detail/server/HttpResponseHead.h"
#include "ruvia/http/detail/server/HttpResponseStreamHead.h"
#include "ruvia/http/detail/http1/Http1ChunkedFraming.h"
#include "ruvia/http/detail/http1/Http1ServerSemantics.h"
#include "ruvia/web/detail/server/HttpResponseStreamState.h"
#include "ruvia/core/Task.h"
#include "ruvia/web/Context.h"
#include "ruvia/http/HttpTypes.h"
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
        ResponseStreamKind kind,
        Http1ResponseStreamPlan plan) noexcept
        : stream_(stream),
          head_(head),
          scratch_(memory.resource()),
          trailers_(memory.resource()),
          scannerEntry_(scannerEntry),
          kind_(kind),
          plan_(plan),
          connectionPlan_(plan.requestConnectionPlan().requireClose()) {}

    [[nodiscard]] bool committed() const noexcept { return state_.committed(); }

    [[nodiscard]] const ResponseStreamCommitPlan* commitPlan() const noexcept {
        return state_.commitPlan();
    }

    [[nodiscard]] bool aborted() const noexcept { return aborted_; }

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
    friend void responseStreamBindContextThunk(void*, Context*, ResponseStreamState::StreamingHeadThunk) noexcept;
    template <typename Sink>
    friend std::pmr::string& responseStreamScratchThunk(void*) noexcept;

private:
    void bindContext(Context* context, ResponseStreamState::StreamingHeadThunk streamingHead) noexcept {
        state_.bindContext(context, streamingHead);
    }

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

        auto streamHead = prepareHttp1ResponseStreamHead(
            state_.streamingHead(),
            kind_,
            plan_,
            trailerIntent);
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
        auto ec = co_await asyncError([this, headView = head_.view()](auto handler) mutable {
            asio::async_write(stream_, asio::buffer(headView), std::move(handler));
        });
        if (ec) {
            aborted_ = true;
            throw std::system_error(ec);
        }
        scannerEntry_.touch();
    }

    Task<void> sleep(std::chrono::milliseconds duration) {
        asio::steady_timer timer(stream_.get_executor(), duration);
        const auto ec = co_await asyncError([&timer](auto handler) mutable {
            timer.async_wait(std::move(handler));
        });
        if (ec) {
            throw std::system_error(ec);
        }
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
            const auto rawEc = co_await asyncError([this, chunk](auto handler) mutable {
                asio::async_write(stream_, asio::buffer(chunk), std::move(handler));
            });
            if (rawEc) {
                aborted_ = true;
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
        const auto writeEc = co_await asyncError([this, &buffers](auto handler) mutable {
            asio::async_write(stream_, buffers, std::move(handler));
        });
        if (writeEc) {
            aborted_ = true;
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

        const auto trailerIntent = trailers.empty()
            ? ResponseTrailerIntent::kNone
            : ResponseTrailerIntent::kPresent;
        if (!trailers.empty()) {
            if (!responseTrailerSectionValid(trailers)) {
                throw std::invalid_argument("invalid response trailer section");
            }
            clearPmrStringRetainingSmall(trailers_);
            for (const auto& trailer : trailers) {
                appendHttp1TrailerField(
                    trailers_, trailer.name(), trailer.value());
            }
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
        const auto ec = co_await asyncError([this, &buffers](auto handler) mutable {
            asio::async_write(stream_, buffers, std::move(handler));
        });
        state_.markEnded();
        if (ec) {
            aborted_ = true;
            throw std::system_error(ec);
        }
        scannerEntry_.touch();
    }

    Stream& stream_;
    ResponseHeadBuffer& head_;
    std::pmr::string scratch_;
    std::pmr::string trailers_;
    ScannerEntry& scannerEntry_;
    ResponseStreamKind kind_;
    Http1ResponseStreamPlan plan_;
    Http1ServerConnectionPlan connectionPlan_;
    ResponseStreamState state_;
    bool aborted_{false};
};

}  // namespace ruvia::detail
