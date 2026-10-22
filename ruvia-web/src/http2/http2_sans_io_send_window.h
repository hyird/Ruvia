#pragma once

#include <cstdint>
#include <string_view>
#include <type_traits>

#include "ruvia/core/task.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http2_connection.h"

#include "http2/http2_data_output_budget.h"
#include "http2/http2_sans_io_stream_signal.h"

namespace ruvia::detail {

enum class http2_send_window_wait_status : std::uint8_t {
    ready,
    aborted,
};

// Wait until the HTTP core no longer owns queued DATA for this stream. The
// Web-owned signal is only a wakeup edge; stream existence, abort, session end,
// and queue state are re-checked together after every wake.
[[nodiscard]] task<http2_send_window_wait_status> await_http2_send_window(
    ruvia::http2_connection& connection, std::uint32_t stream_id,
    http2_sans_io_stream_signal* signal, const bool* aborted = nullptr);

enum class http2_stream_data_write_status : std::uint8_t {
    completed,
    local_aborted,
    session_terminated,
    peer_closed,
    invalid_state,
    content_length_exceeded,
    content_length_incomplete,
};

[[nodiscard]] task<http2_stream_data_write_status> write_http2_stream_data_chunk(
    ruvia::http2_connection& connection, std::uint32_t stream_id,
    http2_sans_io_stream_signal* signal, worker_signal& write_signal,
    http2_data_output_budget& output_budget, std::string_view chunk,
    http2_end_stream end_stream, const bool* aborted = nullptr);

[[nodiscard]] task<http2_stream_data_write_status> write_http2_stream_data(
    ruvia::http2_connection& connection, std::uint32_t stream_id,
    http2_sans_io_stream_signal* signal, worker_signal& write_signal,
    http2_data_output_budget& output_budget, std::string_view bytes,
    http2_end_stream end_stream, const bool* aborted = nullptr);

}  // namespace ruvia::detail
