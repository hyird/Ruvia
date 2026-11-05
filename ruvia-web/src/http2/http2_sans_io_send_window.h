#pragma once

#include <cstdint>
#include <string_view>

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

// Wait for payload window availability. Input processing wakes the Web-owned
// signal even when SETTINGS/WINDOW_UPDATE produces no application event.
// Stream existence, abort and session end are re-checked after every wake.
// The session resets a stream whose wait outlasts write_timeout_ (Pingora
// bounds the h2 capacity wait by its write timeout), which ends it as aborted.
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

// Submits caller-owned input with a credit-sized payload allowance, retaining
// each unconsumed suffix until window and output credit are available.
// Empty END_STREAM bypasses payload credit. The caller keeps bytes alive.
[[nodiscard]] task<http2_stream_data_write_status> write_http2_stream_data(
    ruvia::http2_connection& connection, std::uint32_t stream_id,
    http2_sans_io_stream_signal* signal, worker_signal& write_signal,
    http2_data_output_budget& output_budget, std::string_view bytes,
    http2_end_stream end_stream, const bool* aborted = nullptr);

}  // namespace ruvia::detail
