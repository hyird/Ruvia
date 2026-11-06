#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "ruvia/core/task.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http_status.h"

#include "server/response_file_source.h"

namespace ruvia {
class http_response;
class http_buffered_response_write_plan;
}  // namespace ruvia

namespace ruvia::detail {

class http3_session;
class http3_stream;
class http3_stream_signal;

// Streams of one connection that may open or read a file body on the blocking
// pool at once; the rest wait holding no read buffer and no pool task.
inline constexpr std::size_t http3_file_read_slots = 4;

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
    http3_session& session, std::int64_t stream_id, http3_stream_signal& signal, std::string_view bytes);

struct http3_buffered_response_write_result final {
    // The final status, once the response head was submitted.
    std::optional<http_status_code> committed_status_{};
    // Why a file body failed. Without committed_status_ nothing of the
    // response was sent and the stream still awaits one.
    std::optional<response_file_failure> file_failure_{};
};

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
    // Releases its file read slot when the stream's read and submission end,
    // however the writing coroutine leaves.
    class file_read_slot final {
    public:
        explicit file_read_slot(http3_buffered_response_writer& owner) noexcept
            : owner_(&owner) {}
        ~file_read_slot();

        file_read_slot(const file_read_slot&) = delete;
        file_read_slot& operator=(const file_read_slot&) = delete;

    private:
        http3_buffered_response_writer* owner_;
    };

    [[nodiscard]] task<http3_buffered_response_write_result> write_file_body(
        std::int64_t stream_id, http3_stream_signal& signal, response_file_source& source,
        http_status_code committed_status);
    [[nodiscard]] task<bool> acquire_file_read(std::int64_t stream_id);
    [[nodiscard]] bool stream_ended(std::int64_t stream_id) noexcept;

    http3_session& session_;
    response_file_io file_io_;
    std::size_t file_reads_{0};
    worker_signal file_read_released_;
};

}  // namespace ruvia::detail
