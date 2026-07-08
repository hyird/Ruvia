#pragma once

#include <chrono>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <asio.hpp>

#include "Http2ResponseHeaders.h"
#include "Http2StreamState.h"
#include "runtime/AsioAwait.h"
#include "net/server/HttpResponseStreamHead.h"
#include "net/server/HttpResponseStreamState.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/http/detail/PmrString.h"

namespace ruvia {
class Context;  // only stored/forwarded as Context* (type-erased bindContext); web supplies the definition
}

namespace ruvia::detail {

template <typename Session>
class Http2ResponseStreamSink final {
public:
    Http2ResponseStreamSink(
        Session& session,
        Http2StreamState& stream,
        ResponseBodyMode mode) noexcept
        : session_(session),
          stream_(stream),
          mode_(mode),
          scratch_(session.memory_.resource()),
          trailers_(session.memory_.resource()),
          lowerName_(session.memory_.resource()) {}

    [[nodiscard]] bool committed() const noexcept {
        return state_.committed();
    }

    [[nodiscard]] bool aborted() const noexcept {
        return stream_.isReset();
    }

    template <typename Sink>
    friend Task<void> responseStreamWriteThunk(void*, std::string_view);
    template <typename Sink>
    friend Task<void> responseStreamEndThunk(void*);
    template <typename Sink>
    friend Task<void> responseStreamSleepThunk(void*, std::chrono::milliseconds);
    template <typename Sink>
    friend void responseStreamAddTrailerThunk(void*, std::string_view, std::string_view);
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

    Task<void> commit() {
        if (state_.committed()) {
            co_return;
        }

        auto streamHead = prepareResponseStreamHead(
            state_.streamingHead(),
            mode_,
            ResponseStreamFraming::kHttp2DataFrames);
        appendHttp2ResponseHeaders(stream_, streamHead.response(), 0, false);
        state_.markCommitted(streamHead.bodyForbidden());
        co_await session_.writeHeaders(stream_, stream_.responseHeaderBlock(), state_.bodyForbidden());
        http2ReleaseResponseHeaderBlock(stream_);
        if (state_.bodyForbidden()) {
            state_.markEnded();
        }
    }

    Task<void> write(std::string_view chunk) {
        if (chunk.empty()) {
            co_return;
        }
        co_await commit();
        state_.ensureBodyAllowed();
        co_await session_.writeData(stream_, chunk, {}, false);
    }

    Task<void> sleep(std::chrono::milliseconds duration) {
        asio::steady_timer timer(session_.socket_.get_executor(), duration);
        const auto ec = co_await asyncError([&timer](auto handler) mutable {
            timer.async_wait(std::move(handler));
        });
        if (ec) {
            throw std::system_error(ec);
        }
    }

    // RFC 9113 Section 8.1 trailers are queued before the stream ends and HPACK
    // encoded into a header block flushed here as a trailing HEADERS frame that
    // carries END_STREAM, in place of the empty END_STREAM DATA frame.
    void addTrailer(std::string_view name, std::string_view value) {
        state_.ensureTrailerAllowed(name, value);
        lowerName_.clear();
        lowerName_.reserve(name.size());
        for (const char ch : name) {
            lowerName_.push_back(static_cast<char>(asciiToLower(static_cast<unsigned char>(ch))));
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
            co_await session_.writeData(stream_, {}, {}, true);
        } else {
            co_await session_.writeHeaders(stream_, trailers_, true);
        }
        state_.markEnded();
    }

    Session& session_;
    Http2StreamState& stream_;
    ResponseBodyMode mode_;
    ResponseStreamState state_;
    std::pmr::string scratch_;
    std::pmr::string trailers_;
    std::pmr::string lowerName_;
};

}  // namespace ruvia::detail
