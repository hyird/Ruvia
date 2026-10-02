#pragma once

#include <chrono>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>

#include "ruvia/core/Timer.h"
#include "ruvia/http/Http3DataWritePlan.h"
#include "ruvia/http/Http3ResponseWriter.h"
#include "ruvia/web/detail/server/response/HttpStreamingResponseCompression.h"
#include "ruvia/web/detail/server/stream/HttpResponseStreamState.h"

namespace ruvia::detail {
class Http3BufferedRequestDispatch;

// One worker-owned response stream. The publisher parks on the connection's
// bounded output mailbox; neither the sink nor the producer owns QUIC state.
class Http3ResponseStreamSink final {
public:
    Http3ResponseStreamSink(Http3BufferedRequestDispatch& publisher,
        const WorkerHandle& worker, HttpKnownMethod method, ResponseStreamKind kind,
        std::pmr::memory_resource* resource, HttpResponseCodingSelection coding,
        HttpResponseCodingAvailability availability);
    [[nodiscard]] bool committed() const noexcept {
        return state_.committed();
    }
    [[nodiscard]] const ResponseStreamCommitPlan* commitPlan() const& noexcept {
        return state_.commitPlan();
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
    [[nodiscard]] Task<void> commit(ResponseTrailerIntent trailers);
    [[nodiscard]] Task<void> writeEncoded(std::string_view bytes);
    void requireActive() const;
    Http3BufferedRequestDispatch& publisher_;
    const WorkerHandle& worker_;
    HttpKnownMethod method_;
    ResponseStreamKind kind_;
    std::pmr::memory_resource* resource_;
    ResponseStreamState state_;
    HttpStreamingResponseCompression compression_;
    std::optional<Http3DataWritePlan> data_;
};
}  // namespace ruvia::detail
