#include "http2/http2_buffered_response_write.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"

#include "http2/http2_sans_io_send_window.h"
#include "http2/http2_sans_io_session_engine.h"
#include "http2/http2_sans_io_stream_runtime.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] bool data_write_failed(http2_stream_data_write_status status) noexcept {
    switch (status) {
        case http2_stream_data_write_status::completed:
        case http2_stream_data_write_status::local_aborted:
        case http2_stream_data_write_status::session_terminated:
        case http2_stream_data_write_status::peer_closed:
            return false;
        case http2_stream_data_write_status::invalid_state:
        case http2_stream_data_write_status::content_length_exceeded:
            return true;
    }
    return true;
}

}  // namespace

http2_buffered_response_writer::http2_buffered_response_writer(http2_sans_io_session_engine& session,
    ruvia::http2_connection& connection, http2_sans_io_stream_runtime_table& stream_runtimes,
    worker_signal& write_signal, http2_data_output_budget& output_budget, response_file_io file_io) noexcept
    : session_(session),
      connection_(connection),
      stream_runtimes_(stream_runtimes),
      write_signal_(write_signal),
      output_budget_(output_budget),
      file_io_(std::move(file_io)),
      file_reads_(file_io_.worker_, http2_file_read_slots) {}
void http2_buffered_response_writer::wake_writer() noexcept {
    write_signal_.notify();
}

bool http2_buffered_response_writer::stream_ended(std::uint32_t stream_id) const noexcept {
    if (connection_.stream_aborted(stream_id)) {
        return true;
    }
    auto* runtime = stream_runtimes_.find(stream_id);
    auto* signal = runtime != nullptr ? runtime->signal() : nullptr;
    return signal != nullptr && signal->terminated();
}

task<http2_stream_data_write_status> http2_buffered_response_writer::write_data(
    std::uint32_t stream_id, std::string_view chunk, http2_end_stream end_stream) {
    auto* runtime = stream_runtimes_.find(stream_id);
    auto* signal = runtime != nullptr ? runtime->signal() : nullptr;
    return write_http2_stream_data(
        connection_, stream_id, signal, write_signal_, output_budget_, chunk, end_stream);
}

task<multiplexed_buffered_response_write_result> http2_buffered_response_writer::write_file_body(
    std::uint32_t stream_id, response_file_source& source, http_status_code committed_status) {
    const auto fail_after_commit = [this, stream_id]() noexcept {
        session_.reset_stream_no_throw(stream_id, http2_error_code::internal_error);
    };
    wake_writer();
    for (;;) {
        std::optional<multiplexed_file_read_limiter::slot> slot;
        std::size_t max_bytes = response_file_read_ahead_bytes;
        if (source.file_bytes_pending()) {
            // Read only what the peer has granted: a stream without send
            // window holds neither a read buffer nor a pool task.
            auto* runtime = stream_runtimes_.find(stream_id);
            if (co_await await_http2_send_window(
                    connection_, stream_id, runtime != nullptr ? runtime->signal() : nullptr) ==
                http2_send_window_wait_status::aborted) {
                co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status};
            }
            if (!(co_await file_reads_.acquire(
                    [this, stream_id]() noexcept { return stream_ended(stream_id); }))) {
                co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status};
            }
            slot.emplace(file_reads_);
            if (stream_ended(stream_id)) {
                co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status};
            }
            const auto window = connection_.send_window_state(stream_id);
            if (!window) {
                co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status};
            }
            if (window->available_ == 0) {
                // SETTINGS shrank the window while this stream waited for a
                // slot: give the slot back and wait again.
                continue;
            }
            max_bytes = window->available_;
        }
        const auto chunk = co_await source.next(max_bytes);
        if (chunk.kind() == response_body_chunk::kind_type::failed) {
            // The committed Content-Length can no longer be honoured.
            fail_after_commit();
            co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status, .file_failure_ = *source.failure()};
        }
        if (chunk.kind() == response_body_chunk::kind_type::end) {
            if (co_await write_data(stream_id, {}, http2_end_stream::end_stream) != http2_stream_data_write_status::completed) {
                fail_after_commit();
            }
            co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status};
        }
        const auto bytes = chunk.bytes();
        if (connection_.stream_aborted(stream_id)) {
            co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status};
        }
        const auto result_value = co_await write_data(stream_id, bytes, http2_end_stream::keep_open);
        source.release_buffer();
        if (result_value != http2_stream_data_write_status::completed) {
            if (data_write_failed(result_value)) {
                fail_after_commit();
            }
            co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status};
        }
    }
}

task<multiplexed_buffered_response_write_result> http2_buffered_response_writer::write(
    std::uint32_t stream_id, const http_response& response, http_buffered_response_write_plan write_plan) {
    if (stream_ended(stream_id)) {
        co_return multiplexed_buffered_response_write_result{};
    }

    std::optional<response_file_source> source;
    // Open and validate the file before HEADERS, without reading it: a file
    // that cannot be served still gets an error response, and a stream
    // without window holds no buffer. HEAD and status-suppressed bodies do not
    // transfer a representation.
    if (!write_plan.body_plan().body_suppressed() &&
        (response.file_body() || response.has_multipart_file_body())) {
        if (const auto open_failure = co_await file_reads_.open_file_body(
                source, response, file_io_,
                [this, stream_id]() noexcept { return stream_ended(stream_id); })) {
            co_return *open_failure;
        }
    }

    const auto head_result =
        connection_.submit_response_head(stream_id, response, std::move(write_plan));
    const auto* submitted_head = std::get_if<http_buffered_response_write_plan>(&head_result);
    if (submitted_head == nullptr) {
        const auto error = std::get<http2_response_head_submit_error>(head_result);
        if (error == http2_response_head_submit_error::closed) {
            co_return multiplexed_buffered_response_write_result{};
        }
        // Invalid final metadata cannot leave an open peer stream waiting for a
        // response that the transactional head submission rejected.
        session_.reset_stream_no_throw(stream_id,
            error == http2_response_head_submit_error::peer_stream_limit_reached
                ? http2_error_code::cancel
                : http2_error_code::internal_error);
        co_return multiplexed_buffered_response_write_result{};
    }

    auto* runtime = stream_runtimes_.find(stream_id);
    auto* signal = runtime != nullptr ? runtime->signal() : nullptr;
    if (runtime != nullptr) {
        runtime->mark_response_committed();
    }
    const auto committed_status = submitted_head->body_plan().response_status();
    const auto fail_after_commit = [this, stream_id, committed_status]() noexcept -> multiplexed_buffered_response_write_result {
        session_.reset_stream_no_throw(stream_id, http2_error_code::internal_error);
        return {.committed_status_ = committed_status};
    };
    if (!submitted_head->send_body()) {
        wake_writer();
        co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status};
    }

    if (source) {
        co_return co_await write_file_body(stream_id, *source, committed_status);
    }

    // Keep the first DATA submission synchronous: a response fitting the
    // current window and one output credit needs no DATA coroutine.
    auto body = response.body_bytes();
    if (!body.empty() && signal != nullptr && !signal->terminated() && !connection_.stream_aborted(stream_id)) {
        const auto window = connection_.send_window_state(stream_id);
        if (window && window->available_ != 0 && output_budget_.try_acquire(stream_id)) {
            const auto submitted = submit_http2_stream_data(
                connection_, stream_id, write_signal_, output_budget_, body, http2_end_stream::end_stream);
            if (submitted.failure_) {
                co_return fail_after_commit();
            }
            body.remove_prefix(submitted.progress_.consumed_bytes_);
            if (submitted.progress_.status_ == http2_data_submit_status::complete ||
                submitted.progress_.status_ == http2_data_submit_status::closed) {
                co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status};
            }
        } else {
            wake_writer();
        }
    } else {
        wake_writer();
    }
    if (!body.empty()) {
        const auto result_value = co_await write_data(stream_id, body, http2_end_stream::end_stream);
        if (data_write_failed(result_value)) {
            co_return fail_after_commit();
        }
    }

    // An empty write plan committed END_STREAM with the response head above.
    co_return multiplexed_buffered_response_write_result{.committed_status_ = committed_status};
}

}  // namespace ruvia::detail
