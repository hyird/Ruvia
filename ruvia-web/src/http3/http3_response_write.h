#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "ruvia/core/task.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http_status.h"

#include "server/multiplexed_stream_signal.h"
#include "server/response_file_source.h"

namespace ruvia {
class http_response;
class http_buffered_response_write_plan;
}  // namespace ruvia

namespace ruvia::detail {

class http3_session;
class http3_stream;
// Streams of one connection that may open or read a file body on the blocking
// pool at once; the rest wait holding no read buffer and no pool task.
inline constexpr std::size_t http3_file_read_slots = multiplexed_file_read_slots;

enum class http3_data_write_status : std::uint8_t {
    completed,
    // The stream was reset (by either side) or stopped.
    stream_aborted,
    // The connection ended.
    terminated,
};

// Submits all of `bytes` as DATA of a stream, at most the stream's output
// budget at a time (http3_connection::submit_data): when the budget is
// spent it waits for the peer's acknowledgements, timed by the session
// against write_timeout_. A Content-Length overrun throws std::length_error.
[[nodiscard]] task<http3_data_write_status> write_http3_stream_data(
    http3_session& session, std::int64_t stream_id, multiplexed_stream_signal& signal, std::string_view bytes);

using http3_buffered_response_write_result = multiplexed_buffered_response_write_result;

// Writes a buffered or file response on one request stream: the head with its
// write plan's Content-Length, then the content within the stream's output
// budget. A file body is opened and validated before the head without being
// read; each later chunk is read only once output budget is free, at most that
// much, while holding one of http3_file_read_slots through its submission. A
// stream it cannot complete is reset through the session.
class http3_buffered_response_writer final {
public:
    http3_buffered_response_writer(http3_session& session, response_file_io file_io) noexcept;

    http3_buffered_response_writer(const http3_buffered_response_writer&) = delete;
    http3_buffered_response_writer& operator=(const http3_buffered_response_writer&) = delete;
    http3_buffered_response_writer(http3_buffered_response_writer&&) = delete;
    http3_buffered_response_writer& operator=(http3_buffered_response_writer&&) = delete;

    [[nodiscard]] task<http3_buffered_response_write_result> write(std::int64_t stream_id,
        const http_response& response, http_buffered_response_write_plan write_plan);

private:
    [[nodiscard]] task<http3_buffered_response_write_result> write_file_body(
        std::int64_t stream_id, multiplexed_stream_signal& signal, response_file_source& source,
        http_status_code committed_status);
    [[nodiscard]] bool stream_ended(std::int64_t stream_id) noexcept;

    http3_session& session_;
    response_file_io file_io_;
    multiplexed_file_read_limiter file_reads_;
};

}  // namespace ruvia::detail
