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
    // queued transfers the unsent suffix to the core; backpressured retains
    // caller ownership and must retry this exact stable view after the older
    // queued input drains.
    for (;;) {
        auto* runtime = stream_runtimes_.find(stream_id);
        auto* signal = runtime != nullptr ? runtime->signal() : nullptr;
        if (signal != nullptr && signal->terminated()) {
            co_return data_write_result_type::failed;
        }
        if (signal == nullptr) {
            co_return data_write_result_type::peer_aborted;
        }
        for (;;) {
            const auto window = connection_.send_window_state(stream_id);
            if (!window) {
                co_return data_write_result_type::peer_aborted;
            }
            if (window->available_ != 0) {
                break;
            }
            co_await output_budget_.wait_for_change();
            if (signal->terminated()) {
                co_return data_write_result_type::peer_aborted;
            }
        }
        if (!(co_await output_budget_.acquire(stream_id, *signal))) {
            co_return data_write_result_type::peer_aborted;
        }
        const auto result_value = connection_.submit_data(stream_id, chunk, end_stream);
        if (result_value == http2_data_submit_status::accepted || result_value == http2_data_submit_status::queued) {
            output_budget_.note_data_submitted(stream_id, chunk.size());
        }
        wake_writer();
        if (result_value != http2_data_submit_status::accepted && result_value != http2_data_submit_status::queued) {
            output_budget_.release(stream_id);
        }
        if (result_value == http2_data_submit_status::accepted) {
            co_return data_write_result_type::completed;
        }
        if (result_value == http2_data_submit_status::closed) {
            co_return data_write_result_type::peer_aborted;
        }
        if (result_value == http2_data_submit_status::invalid_state ||
            result_value == http2_data_submit_status::content_length_exceeded ||
            result_value == http2_data_submit_status::content_length_incomplete) {
            co_return data_write_result_type::failed;
        }
        const auto wait_result = co_await await_http2_send_window(connection_, stream_id, signal);
        if (wait_result.aborted() != nullptr) {
            co_return data_write_result_type::peer_aborted;
        }
        if (result_value == http2_data_submit_status::queued) {
            co_return data_write_result_type::completed;
        }
    }
}

task<http2_buffered_response_write_result> http2_buffered_response_writer::write(
    std::uint32_t stream_id, const http_response& response, http_buffered_response_write_plan write_plan) {
    if (connection_.stream_aborted(stream_id)) {
        co_return http2_buffered_response_write_result::make_peer_aborted_before_commit();
    }
    auto* runtime = stream_runtimes_.find(stream_id);
    auto* signal = runtime != nullptr ? runtime->signal() : nullptr;
    if (signal != nullptr && signal->terminated()) {
        co_return http2_buffered_response_write_result::make_failed_before_commit();
    }

    const auto head_result =
        connection_.submit_response_head(stream_id, response, std::move(write_plan));
    const auto* submitted_head = head_result.submitted();
    if (submitted_head == nullptr) {
        if (head_result.failure()->error() == http2_response_head_submit_error::closed) {
            co_return http2_buffered_response_write_result::make_peer_aborted_before_commit();
        }
        // Invalid final metadata cannot leave an open peer stream waiting for a
        // response that the transactional head submission rejected.
        submit_reset_no_throw(connection_, stream_id, http2_error_code::internal_error);
        wake_writer();
        co_return http2_buffered_response_write_result::make_failed_before_commit();
    }

    wake_writer();
    const auto committed_status = submitted_head->response_status();
    const auto fail_after_commit = [this, stream_id, committed_status]() noexcept {
        submit_reset_no_throw(connection_, stream_id, http2_error_code::internal_error);
        wake_writer();
        return http2_buffered_response_write_result::make_failed_after_commit(committed_status);
    };
    if (!submitted_head->send_body()) {
        co_return http2_buffered_response_write_result::make_completed(committed_status);
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
                co_return http2_buffered_response_write_result::make_completed(committed_status);
            }
            if (chunk.kind() == response_body_chunk::kind_type::failed) {
                // The committed Content-Length can no longer be honoured.
                co_return fail_after_commit();
            }
            const auto bytes = chunk.bytes();
            for (std::size_t offset = 0; offset < bytes.size();) {
                if (connection_.stream_aborted(stream_id)) {
                    co_return http2_buffered_response_write_result::make_peer_aborted_after_commit(committed_status);
                }
                const auto size = std::min<std::size_t>(slice_bytes, bytes.size() - offset);
                const auto result_value = co_await write_data(stream_id, bytes.substr(offset, size),
                    http2_end_stream::keep_open);
                if (result_value == data_write_result_type::peer_aborted) {
                    co_return http2_buffered_response_write_result::make_peer_aborted_after_commit(committed_status);
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
            co_return http2_buffered_response_write_result::make_peer_aborted_after_commit(committed_status);
        }
        if (result_value == data_write_result_type::failed) {
            co_return fail_after_commit();
        }
        offset += size;
    }

    // An empty write plan committed END_STREAM with the response head above.
    co_return http2_buffered_response_write_result::make_completed(committed_status);
}

}  // namespace ruvia::detail
