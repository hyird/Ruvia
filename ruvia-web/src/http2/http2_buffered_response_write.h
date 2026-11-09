#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

#include "ruvia/core/task.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http2_connection.h"

#include "http2/http2_data_output_budget.h"
#include "server/multiplexed_stream_signal.h"
#include "server/response_file_source.h"

namespace ruvia {
class http_response;
class http_buffered_response_write_plan;
}  // namespace ruvia

namespace ruvia::detail {
class http2_sans_io_session_engine;
class http2_sans_io_stream_runtime_table;

// Streams of one connection that may open or read a file body on the blocking
// pool at once. The rest wait holding no read buffer and no pool task. Equal
// to the DATA output credit slots: more concurrent reads could not be
// submitted any sooner.
inline constexpr std::size_t http2_file_read_slots = http2_data_output_credit_slots;

using http2_buffered_response_write_result = multiplexed_buffered_response_write_result;

// Non-transport HTTP/2 buffered/file response driver. The session template owns
// socket reads/writes and event dispatch; this object owns the single response
// commit, DATA backpressure, the file body (through response_file_source), and
// the typed terminal result chain. Keeping it non-template compiles that policy
// once for plain and TLS sessions. A stream it cannot complete is reset through
// the owning session's single reset entry point.
//
// A file body is validated before HEADERS without being read. Each later
// chunk is read only after the peer granted send window, at most that much,
// while holding one of http2_file_read_slots through its DATA submission.
class http2_buffered_response_writer final {
public:
    http2_buffered_response_writer(http2_sans_io_session_engine& session, ruvia::http2_connection& connection,
        http2_sans_io_stream_runtime_table& stream_runtimes, worker_signal& write_signal,
        http2_data_output_budget& output_budget, response_file_io file_io) noexcept;

    http2_buffered_response_writer(const http2_buffered_response_writer&) = delete;
    http2_buffered_response_writer& operator=(const http2_buffered_response_writer&) = delete;
    http2_buffered_response_writer(http2_buffered_response_writer&&) = delete;
    http2_buffered_response_writer& operator=(http2_buffered_response_writer&&) = delete;

    [[nodiscard]] task<http2_buffered_response_write_result> write(std::uint32_t stream_id,
        const http_response& response, http_buffered_response_write_plan write_plan);

private:
    enum class data_write_result_type : std::uint8_t { completed,
        peer_aborted,
        failed };

    [[nodiscard]] task<data_write_result_type> write_data(
        std::uint32_t stream_id, std::string_view chunk, http2_end_stream end_stream);
    [[nodiscard]] task<http2_buffered_response_write_result> write_file_body(
        std::uint32_t stream_id, response_file_source& source, http_status_code committed_status);
    [[nodiscard]] bool stream_ended(std::uint32_t stream_id) const noexcept;
    void wake_writer() noexcept;

    http2_sans_io_session_engine& session_;
    ruvia::http2_connection& connection_;
    http2_sans_io_stream_runtime_table& stream_runtimes_;
    worker_signal& write_signal_;
    http2_data_output_budget& output_budget_;
    response_file_io file_io_;
    multiplexed_file_read_limiter file_reads_;
};

}  // namespace ruvia::detail
