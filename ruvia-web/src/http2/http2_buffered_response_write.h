#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

#include "ruvia/core/task.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http2_connection.h"

#include "http2/http2_data_output_budget.h"
#include "server/response_file_source.h"

namespace ruvia {
class http_response;
class http_buffered_response_write_plan;
}  // namespace ruvia

namespace ruvia::detail {
class http2_sans_io_stream_runtime_table;

// The writer completes transport recovery (including RESET_STREAM) before
// returning. committed_status_ is present whenever the response head was
// committed to the connection.
struct http2_buffered_response_write_result final {
    std::optional<http_status_code> committed_status_{};
};
// Non-transport HTTP/2 buffered/file response driver. The session template owns
// socket reads/writes and event dispatch; this object owns the single response
// commit, DATA backpressure, the file body (through response_file_source), and
// the typed terminal result chain. Keeping it non-template compiles that policy
// once for plain and TLS sessions.
class http2_buffered_response_writer final {
public:
    http2_buffered_response_writer(ruvia::http2_connection& connection,
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
    void wake_writer() noexcept;

    ruvia::http2_connection& connection_;
    http2_sans_io_stream_runtime_table& stream_runtimes_;
    worker_signal& write_signal_;
    http2_data_output_budget& output_budget_;
    response_file_io file_io_;
};

}  // namespace ruvia::detail
