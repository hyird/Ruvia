#pragma once

#include <chrono>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>

#include "ruvia/core/Timer.h"
#include "ruvia/http/Http3DataWritePlan.h"
#include "ruvia/http/Http3ResponseWriter.h"
#include "ruvia/http/HttpResponseStream.h"

#include "server/HttpResponseStreamState.h"
#include "server/HttpStreamingResponseCompression.h"

namespace ruvia::detail {
class Http3BufferedRequestDispatch;

// One worker-owned response stream. The publisher parks on the connection's
// bounded output buffer; neither the sink nor the producer owns QUIC state.
class Http3ResponseStreamSink final {
public:
    Http3ResponseStreamSink(Http3BufferedRequestDispatch& publisher,
        const WorkerHandle& worker, HttpKnownMethod method, http_response_stream_kind kind,
        std::pmr::memory_resource* resource, HttpResponseCodingSelection coding,
        HttpResponseCodingAvailability availability);
    [[nodiscard]] bool committed() const noexcept {
        return state_.committed();
    }
    [[nodiscard]] const http_response_stream_commit_plan* commit_plan() const& noexcept {
        return state_.commit_plan();
    }
    [[nodiscard]] bool aborted() const noexcept;
    void bindContext(Context* context, ResponseStreamState::StreamingHeadThunk head) {
        state_.bindContext(context, head);
    }
    void releaseContext() noexcept {
        state_.releaseContext();
    }
    [[nodiscard]] Task<void> write(std::string_view bytes);
    [[nodiscard]] Task<void> end(std::span<const HttpHeaderView> trailers);
    [[nodiscard]] Task<TimerSleepResult> sleep(std::chrono::milliseconds duration, const StopToken& stop);

private:
    [[nodiscard]] Task<void> commit(http_response_trailer_intent trailers);
    [[nodiscard]] Task<void> writeEncoded(std::string_view bytes);
    void requireActive() const;
    Http3BufferedRequestDispatch& publisher_;
    const WorkerHandle& worker_;
    HttpKnownMethod method_;
    http_response_stream_kind kind_;
    std::pmr::memory_resource* resource_;
    ResponseStreamState state_;
    HttpStreamingResponseCompression compression_;
    std::optional<Http3DataWritePlan> data_;
};
}  // namespace ruvia::detail
