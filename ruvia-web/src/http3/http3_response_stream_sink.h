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
class http3_buffered_request_dispatch;

// One worker-owned response stream. The publisher parks on the connection's
// bounded output buffer; neither the sink nor the producer owns QUIC state.
class http3_response_stream_sink final {
public:
    http3_response_stream_sink(http3_buffered_request_dispatch& publisher,
        const worker_handle& worker_value, http_known_method method, http_response_stream_kind kind,
        std::pmr::memory_resource* resource, http_response_coding_selection coding,
        http_response_coding_availability availability);
    [[nodiscard]] bool committed() const noexcept {
        return state_.committed();
    }
    [[nodiscard]] const http_response_stream_commit_plan* commit_plan() const& noexcept {
        return state_.commit_plan();
    }
    [[nodiscard]] bool aborted() const noexcept;
    void bind_context(context* context_value, response_stream_state::streaming_head_thunk_type head) {
        state_.bind_context(context_value, head);
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
    http_streaming_response_compression compression_;
    std::optional<http3_data_write_plan> data_;
};
}  // namespace ruvia::detail
