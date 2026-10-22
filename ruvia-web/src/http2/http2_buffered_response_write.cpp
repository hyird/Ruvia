#include "http2/http2_buffered_response_write.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"

#include "http2/http2_sans_io_send_window.h"
#include "http2/http2_sans_io_stream_runtime.h"

namespace ruvia::detail {
namespace {

void submit_reset_no_throw(
    ruvia::http2_connection& connection, std::uint32_t stream_id, http2_error_code error) noexcept {
    try {
        (void)connection.submit_reset(stream_id, error);
    } catch (...) {
        // Failure to serialize RESET_STREAM is already rolled back by
        // http2_connection. This writer reports the local failure through its typed
        // result; the owning session may still retry during stream cleanup.
    }
}

}  // namespace

http2_buffered_response_writer::http2_buffered_response_writer(ruvia::http2_connection& connection,
    http2_sans_io_stream_runtime_table& stream_runtimes, worker_signal& write_signal,
    http2_data_output_budget& output_budget, response_file_io file_io) noexcept
    : connection_(connection),
      stream_runtimes_(stream_runtimes),
      write_signal_(write_signal),
      output_budget_(output_budget),
      file_io_(std::move(file_io)) {}

void http2_buffered_response_writer::wake_writer() noexcept {
    write_signal_.notify();
}

task<http2_buffered_response_writer::data_write_result_type> http2_buffered_response_writer::write_data(
    std::uint32_t stream_id, std::string_view chunk, http2_end_stream end_stream) {
    auto* runtime = stream_runtimes_.find(stream_id);
    auto* signal = runtime != nullptr ? runtime->signal() : nullptr;
    if (signal != nullptr && signal->terminated()) {
        co_return data_write_result_type::failed;
    }
    const auto status = co_await write_http2_stream_data_chunk(
        connection_, stream_id, signal, write_signal_, output_budget_, chunk, end_stream);
    switch (status) {
        case http2_stream_data_write_status::completed:
            co_return data_write_result_type::completed;
        case http2_stream_data_write_status::local_aborted:
        case http2_stream_data_write_status::session_terminated:
        case http2_stream_data_write_status::peer_closed:
            co_return data_write_result_type::peer_aborted;
        case http2_stream_data_write_status::invalid_state:
        case http2_stream_data_write_status::content_length_exceeded:
        case http2_stream_data_write_status::content_length_incomplete:
            co_return data_write_result_type::failed;
    }
    co_return data_write_result_type::failed;
}

task<std::optional<http_status_code>> http2_buffered_response_writer::write(
    std::uint32_t stream_id, const http_response& response, http_buffered_response_write_plan write_plan) {
    if (connection_.stream_aborted(stream_id)) {
        co_return std::nullopt;
    }
    auto* runtime = stream_runtimes_.find(stream_id);
    auto* signal = runtime != nullptr ? runtime->signal() : nullptr;
    if (signal != nullptr && signal->terminated()) {
        co_return std::nullopt;
    }

    const auto head_result =
        connection_.submit_response_head(stream_id, response, std::move(write_plan));
    const auto* submitted_head = std::get_if<http_buffered_response_write_plan>(&head_result);
    if (submitted_head == nullptr) {
        if (std::get<http2_response_head_submit_error>(head_result) == http2_response_head_submit_error::closed) {
            co_return std::nullopt;
        }
        // Invalid final metadata cannot leave an open peer stream waiting for a
        // response that the transactional head submission rejected.
        submit_reset_no_throw(connection_, stream_id, http2_error_code::internal_error);
        wake_writer();
        co_return std::nullopt;
    }

    wake_writer();
    const auto committed_status = submitted_head->response_status();
    const auto fail_after_commit = [this, stream_id, committed_status]() noexcept -> std::optional<http_status_code> {
        submit_reset_no_throw(connection_, stream_id, http2_error_code::internal_error);
        wake_writer();
        return committed_status;
    };
    if (!submitted_head->send_body()) {
        co_return committed_status;
    }

    // Bound the core-owned window-blocked remainder to one frame-sized slice.
    constexpr std::size_t slice_bytes = http2_data_output_credit_bytes;
    if (response.file_body() || response.has_multipart_file_body()) {
        response_file_source source(response, file_io_);
        for (;;) {
            const auto chunk = co_await source.next();
            if (chunk.kind() == response_body_chunk::kind_type::end) {
                const auto ended = co_await write_data(stream_id, {}, http2_end_stream::end_stream);
                if (ended != data_write_result_type::completed) {
                    co_return fail_after_commit();
                }
                co_return committed_status;
            }
            if (chunk.kind() == response_body_chunk::kind_type::failed) {
                // The committed Content-Length can no longer be honoured.
                co_return fail_after_commit();
            }
            const auto bytes = chunk.bytes();
            for (std::size_t offset = 0; offset < bytes.size();) {
                if (connection_.stream_aborted(stream_id)) {
                    co_return committed_status;
                }
                const auto size = std::min<std::size_t>(slice_bytes, bytes.size() - offset);
                const auto result_value = co_await write_data(stream_id, bytes.substr(offset, size),
                    http2_end_stream::keep_open);
                if (result_value == data_write_result_type::peer_aborted) {
                    co_return committed_status;
                }
                if (result_value == data_write_result_type::failed) {
                    co_return fail_after_commit();
                }
                offset += size;
            }
        }
    }

    const auto body = response.body_bytes();
    std::size_t offset = 0;
    while (offset < body.size()) {
        const auto size = std::min<std::size_t>(slice_bytes, body.size() - offset);
        const auto result_value = co_await write_data(stream_id, body.substr(offset, size),
            offset + size == body.size() ? http2_end_stream::end_stream : http2_end_stream::keep_open);
        if (result_value == data_write_result_type::peer_aborted) {
            co_return committed_status;
        }
        if (result_value == data_write_result_type::failed) {
            co_return fail_after_commit();
        }
        offset += size;
    }

    // An empty write plan committed END_STREAM with the response head above.
    co_return committed_status;
}

}  // namespace ruvia::detail
