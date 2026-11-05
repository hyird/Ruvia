#include "http2/http2_buffered_response_write.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
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

using write_result = http2_buffered_response_write_result;

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
      file_read_released_(file_io_.worker_) {}

http2_buffered_response_writer::file_read_slot::~file_read_slot() {
    --owner_->file_reads_;
    owner_->file_read_released_.notify();
}

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

task<bool> http2_buffered_response_writer::acquire_file_read(std::uint32_t stream_id) {
    // Every holder ends its read and submission (or its stream) and notifies,
    // so a waiter wakes to re-check its own stream.
    while (file_reads_ >= http2_file_read_slots) {
        co_await file_read_released_.wait();
        if (stream_ended(stream_id)) {
            co_return false;
        }
    }
    ++file_reads_;
    co_return true;
}

task<http2_buffered_response_writer::data_write_result_type> http2_buffered_response_writer::write_data(
    std::uint32_t stream_id, std::string_view chunk, http2_end_stream end_stream) {
    auto* runtime = stream_runtimes_.find(stream_id);
    auto* signal = runtime != nullptr ? runtime->signal() : nullptr;
    if (signal != nullptr && signal->terminated()) {
        co_return data_write_result_type::failed;
    }
    const auto status = co_await write_http2_stream_data(
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

task<http2_buffered_response_write_result> http2_buffered_response_writer::write_file_body(
    std::uint32_t stream_id, response_file_source& source, http_status_code committed_status) {
    const auto fail_after_commit = [this, stream_id]() noexcept {
        session_.reset_stream_no_throw(stream_id, http2_error_code::internal_error);
    };
    wake_writer();
    for (;;) {
        std::optional<file_read_slot> slot;
        std::size_t max_bytes = response_file_read_ahead_bytes;
        if (source.file_bytes_pending()) {
            // Read only what the peer has granted: a stream without send
            // window holds neither a read buffer nor a pool task.
            auto* runtime = stream_runtimes_.find(stream_id);
            if (co_await await_http2_send_window(
                    connection_, stream_id, runtime != nullptr ? runtime->signal() : nullptr) ==
                http2_send_window_wait_status::aborted) {
                co_return write_result{.committed_status_ = committed_status};
            }
            if (!(co_await acquire_file_read(stream_id))) {
                co_return write_result{.committed_status_ = committed_status};
            }
            slot.emplace(*this);
            if (stream_ended(stream_id)) {
                co_return write_result{.committed_status_ = committed_status};
            }
            const auto window = connection_.send_window_state(stream_id);
            if (!window) {
                co_return write_result{.committed_status_ = committed_status};
            }
            if (window->available_ == 0) {
                // SETTINGS shrank the window while this stream waited for a
                // slot: give the slot back and wait again.
                continue;
            }
            max_bytes = std::min<std::size_t>(max_bytes, window->available_);
        }
        const auto chunk = co_await source.next(max_bytes);
        if (chunk.kind() == response_body_chunk::kind_type::failed) {
            // The committed Content-Length can no longer be honoured.
            fail_after_commit();
            co_return write_result{.committed_status_ = committed_status, .file_failure_ = *source.failure()};
        }
        if (chunk.kind() == response_body_chunk::kind_type::end) {
            if (co_await write_data(stream_id, {}, http2_end_stream::end_stream) != data_write_result_type::completed) {
                fail_after_commit();
            }
            co_return write_result{.committed_status_ = committed_status};
        }
        const auto bytes = chunk.bytes();
        if (connection_.stream_aborted(stream_id)) {
            co_return write_result{.committed_status_ = committed_status};
        }
        const auto result_value = co_await write_data(stream_id, bytes, http2_end_stream::keep_open);
        source.release_buffer();
        if (result_value == data_write_result_type::peer_aborted) {
            co_return write_result{.committed_status_ = committed_status};
        }
        if (result_value == data_write_result_type::failed) {
            fail_after_commit();
            co_return write_result{.committed_status_ = committed_status};
        }
    }
}

task<http2_buffered_response_write_result> http2_buffered_response_writer::write(
    std::uint32_t stream_id, const http_response& response, http_buffered_response_write_plan write_plan) {
    if (stream_ended(stream_id)) {
        co_return write_result{};
    }

    std::optional<response_file_source> source;
    // Open and validate the file before HEADERS, without reading it: a file
    // that cannot be served still gets an error response, and a stream
    // without window holds no buffer. HEAD and status-suppressed bodies do not
    // transfer a representation.
    if (!write_plan.body_plan().body_suppressed() &&
        (response.file_body() || response.has_multipart_file_body())) {
        source.emplace(response, file_io_);
        bool opened = false;
        {
            if (!(co_await acquire_file_read(stream_id))) {
                co_return write_result{};
            }
            const file_read_slot slot(*this);
            opened = co_await source->open_first();
        }
        // The blocking open may have yielded while the peer closed its stream.
        if (stream_ended(stream_id)) {
            co_return write_result{};
        }
        if (!opened) {
            co_return write_result{.file_failure_ = *source->failure()};
        }
    }

    const auto head_result =
        connection_.submit_response_head(stream_id, response, std::move(write_plan));
    const auto* submitted_head = std::get_if<http_buffered_response_write_plan>(&head_result);
    if (submitted_head == nullptr) {
        const auto error = std::get<http2_response_head_submit_error>(head_result);
        if (error == http2_response_head_submit_error::closed) {
            co_return write_result{};
        }
        // Invalid final metadata cannot leave an open peer stream waiting for a
        // response that the transactional head submission rejected.
        session_.reset_stream_no_throw(stream_id,
            error == http2_response_head_submit_error::peer_stream_limit_reached
                ? http2_error_code::cancel
                : http2_error_code::internal_error);
        co_return write_result{};
    }

    auto* runtime = stream_runtimes_.find(stream_id);
    auto* signal = runtime != nullptr ? runtime->signal() : nullptr;
    if (runtime != nullptr) {
        runtime->mark_response_committed();
    }
    const auto committed_status = submitted_head->body_plan().response_status();
    const auto fail_after_commit = [this, stream_id, committed_status]() noexcept -> http2_buffered_response_write_result {
        session_.reset_stream_no_throw(stream_id, http2_error_code::internal_error);
        return {.committed_status_ = committed_status};
    };
    if (!submitted_head->send_body()) {
        wake_writer();
        co_return write_result{.committed_status_ = committed_status};
    }

    if (source) {
        co_return co_await write_file_body(stream_id, *source, committed_status);
    }

    auto body = response.body_bytes();
    if (!body.empty() && signal != nullptr && !signal->terminated() && !connection_.stream_aborted(stream_id)) {
        const auto window = connection_.send_window_state(stream_id);
        if (window && window->available_ != 0 && output_budget_.try_acquire(stream_id)) {
            http2_data_submit_result submitted{};
            try {
                submitted = connection_.submit_data(
                    stream_id, body, http2_end_stream::end_stream, http2_data_output_credit_bytes);
            } catch (const std::logic_error&) {
                output_budget_.release(stream_id);
                co_return fail_after_commit();
            } catch (...) {
                output_budget_.release(stream_id);
                wake_writer();
                throw;
            }
            output_budget_.note_data_submitted(stream_id, submitted.consumed_bytes_);
            if (submitted.consumed_bytes_ == 0) {
                output_budget_.release(stream_id);
            }
            wake_writer();
            body.remove_prefix(submitted.consumed_bytes_);
            if (submitted.status_ == http2_data_submit_status::complete ||
                submitted.status_ == http2_data_submit_status::closed) {
                co_return write_result{.committed_status_ = committed_status};
            }
        } else {
            wake_writer();
        }
    } else {
        wake_writer();
    }
    if (!body.empty()) {
        const auto result_value = co_await write_data(stream_id, body, http2_end_stream::end_stream);
        if (result_value == data_write_result_type::peer_aborted) {
            co_return write_result{.committed_status_ = committed_status};
        }
        if (result_value == data_write_result_type::failed) {
            co_return fail_after_commit();
        }
    }

    // An empty write plan committed END_STREAM with the response head above.
    co_return write_result{.committed_status_ = committed_status};
}

}  // namespace ruvia::detail
