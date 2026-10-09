#include "http2/http2_buffered_response_write.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"

#include "http2/http2_sans_io_send_window.h"
#include "http2/http2_sans_io_stream_runtime.h"
#include "server/http_file_chunk_buffer.h"
#include "server/http_file_open.h"

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
    http2_sans_io_stream_runtime_table& stream_runtimes, worker_memory& worker_value,
    worker_signal& write_signal, http2_data_output_budget& output_budget) noexcept
    : connection_(connection),
      stream_runtimes_(stream_runtimes),
      worker_(worker_value),
      write_signal_(write_signal),
      output_budget_(&output_budget) {}

http2_buffered_response_writer::http2_buffered_response_writer(
    ruvia::http2_connection& connection, http2_sans_io_stream_runtime_table& stream_runtimes,
    worker_memory& worker_value, worker_signal& write_signal) noexcept
    : connection_(connection),
      stream_runtimes_(stream_runtimes),
      worker_(worker_value),
      write_signal_(write_signal) {}

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
        if (output_budget_ != nullptr) {
            for (;;) {
                const auto window = connection_.send_window_state(stream_id);
                if (!window) {
                    co_return data_write_result_type::peer_aborted;
                }
                if (window->available_ != 0) {
                    break;
                }
                co_await output_budget_->wait_for_change();
                if (signal->terminated()) {
                    co_return data_write_result_type::peer_aborted;
                }
            }
            if (!(co_await output_budget_->acquire(stream_id, *signal))) {
                co_return data_write_result_type::peer_aborted;
            }
        }
        const auto result_value = connection_.submit_data(stream_id, chunk, end_stream);
        if (output_budget_ != nullptr &&
            (result_value == http2_data_submit_status::accepted || result_value == http2_data_submit_status::queued)) {
            output_budget_->note_data_submitted(stream_id, chunk.size());
        }
        wake_writer();
        if (result_value != http2_data_submit_status::accepted &&
            result_value != http2_data_submit_status::queued && output_budget_ != nullptr) {
            output_budget_->release(stream_id);
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

    if (response.has_multipart_file_body()) {
        std::pmr::string file_chunk(worker_.allocator<char>());
        ensure_file_chunk_buffer(file_chunk);
        for (std::size_t segment_index = 0; segment_index < response.body_segment_count(); ++segment_index) {
            const auto segment = response.body_segment(segment_index);
            const bool last_segment = segment_index + 1 == response.body_segment_count();
            if (segment.file_) {
                auto input = open_response_file_input(*segment.file_);
                if (!input) {
                    co_return fail_after_commit();
                }
                input.seekg(static_cast<std::streamoff>(segment.file_->offset()), std::ios::beg);
                if (!input) {
                    co_return fail_after_commit();
                }
                std::uint64_t remaining = segment.file_->length();
                while (remaining != 0) {
                    if (connection_.stream_aborted(stream_id)) {
                        co_return http2_buffered_response_write_result::make_peer_aborted_after_commit(committed_status);
                    }
                    const auto next_value = static_cast<std::size_t>(std::min<std::uint64_t>(http2_data_output_credit_bytes, remaining));
                    input.read(file_chunk.data(), static_cast<std::streamsize>(next_value));
                    const auto count = input.gcount();
                    if (count <= 0) {
                        co_return fail_after_commit();
                    }
                    remaining -= static_cast<std::uint64_t>(count);
                    const bool final_chunk = last_segment && remaining == 0;
                    const auto result_value = co_await write_data(stream_id,
                        std::string_view(file_chunk.data(), static_cast<std::size_t>(count)),
                        final_chunk ? http2_end_stream::end_stream : http2_end_stream::keep_open);
                    if (result_value == data_write_result_type::peer_aborted) {
                        co_return http2_buffered_response_write_result::make_peer_aborted_after_commit(committed_status);
                    }
                    if (result_value == data_write_result_type::failed) {
                        co_return fail_after_commit();
                    }
                }
            } else {
                std::size_t offset = 0;
                while (offset < segment.bytes_.size()) {
                    const auto count = std::min<std::size_t>(http2_data_output_credit_bytes, segment.bytes_.size() - offset);
                    const bool final_chunk = last_segment && offset + count == segment.bytes_.size();
                    const auto result_value = co_await write_data(stream_id, segment.bytes_.substr(offset, count),
                        final_chunk ? http2_end_stream::end_stream : http2_end_stream::keep_open);
                    if (result_value == data_write_result_type::peer_aborted) {
                        co_return http2_buffered_response_write_result::make_peer_aborted_after_commit(committed_status);
                    }
                    if (result_value == data_write_result_type::failed) {
                        co_return fail_after_commit();
                    }
                    offset += count;
                }
            }
        }
        co_return http2_buffered_response_write_result::make_completed(committed_status);
    }

    if (const auto file_body = response.file_body()) {
        auto input = open_response_file_input(*file_body);
        bool ready = static_cast<bool>(input);
        if (ready) {
            input.seekg(static_cast<std::streamoff>(file_body->offset()), std::ios::beg);
            ready = static_cast<bool>(input);
        }
        if (!ready) {
            // The committed Content-Length can no longer be honoured.
            co_return fail_after_commit();
        }

        std::pmr::string file_chunk(worker_.allocator<char>());
        ensure_file_chunk_buffer(file_chunk);
        std::uint64_t remaining = file_body->length();
        while (remaining > 0) {
            if (connection_.stream_aborted(stream_id)) {
                co_return http2_buffered_response_write_result::make_peer_aborted_after_commit(
                    committed_status);
            }
            constexpr std::uint64_t data_credit_bytes = http2_data_output_credit_bytes;
            const auto next_value = static_cast<std::size_t>(
                std::min<std::uint64_t>(data_credit_bytes, remaining));
            input.read(file_chunk.data(), static_cast<std::streamsize>(next_value));
            const auto read_bytes = input.gcount();
            if (read_bytes <= 0) {
                co_return fail_after_commit();
            }
            remaining -= static_cast<std::uint64_t>(read_bytes);
            const auto result_value = co_await write_data(stream_id,
                std::string_view(file_chunk.data(), static_cast<std::size_t>(read_bytes)),
                remaining == 0 ? http2_end_stream::end_stream : http2_end_stream::keep_open);
            if (result_value == data_write_result_type::peer_aborted) {
                co_return http2_buffered_response_write_result::make_peer_aborted_after_commit(
                    committed_status);
            }
            if (result_value == data_write_result_type::failed) {
                co_return fail_after_commit();
            }
        }
        co_return http2_buffered_response_write_result::make_completed(committed_status);
    }

    // Bound the core-owned window-blocked remainder to one frame-sized slice.
    const auto body = response.body_bytes();
    constexpr std::size_t slice_bytes = http2_data_output_credit_bytes;
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
