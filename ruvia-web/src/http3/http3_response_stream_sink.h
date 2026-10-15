#pragma once

#include <chrono>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>

#include "ruvia/core/timer.h"
#include "ruvia/http/http3_data_write_plan.h"
#include "ruvia/http/http3_response_writer.h"
#include "ruvia/http/http_response_stream.h"

#include "server/http_response_stream_state.h"
#include "server/http_streaming_response_compression.h"

namespace ruvia::detail {
struct cors_options;
class http3_buffered_request_dispatch;

// One worker-owned response stream. Each write publishes one framed HEADERS
// or DATA frame straight to QUIC and resumes once QUIC accepted all of it.
class http3_response_stream_sink final {
public:
    http3_response_stream_sink(http3_buffered_request_dispatch& publisher,
        const worker_handle& worker_value, http_known_method method, http_response_stream_kind kind,
        std::pmr::memory_resource* resource, worker_codecs& codecs, http_response_coding_selection coding,
        http_response_coding_availability availability, const cors_options* cors);
    [[nodiscard]] bool committed() const noexcept {
        return state_.committed();
    }
    [[nodiscard]] const http_response_stream_commit_plan* commit_plan() const& noexcept {
        return state_.commit_plan();
    }
    [[nodiscard]] bool aborted() const noexcept;
    void bind_context(context* context_value) {
        state_.bind_context(context_value);
    }
    void release_context() noexcept {
        state_.release_context();
    }
    [[nodiscard]] task<void> write(std::string_view bytes);
    [[nodiscard]] task<void> end(std::span<const http_header_view> trailers);
    [[nodiscard]] task<timer_sleep_result> sleep(std::chrono::milliseconds duration, const stop_token& stop);

private:
    [[nodiscard]] task<void> commit(http_response_trailer_intent trailers);
    [[nodiscard]] task<void> write_encoded(std::string_view bytes);
    void require_active() const;
    http3_buffered_request_dispatch& publisher_;
    const worker_handle& worker_;
    http_known_method method_;
    http_response_stream_kind kind_;
    std::pmr::memory_resource* resource_;
    response_stream_state state_;
    const cors_options* cors_;
    http_streaming_response_compression compression_;
    std::optional<http3_data_write_plan> data_;
};
}  // namespace ruvia::detail
