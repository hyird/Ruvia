#include "http3/http3_response_write.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"

#include "http3/http3_session.h"
#include "http3/http3_stream.h"

namespace ruvia::detail {
namespace {

using write_result = http3_buffered_response_write_result;

[[nodiscard]] std::span<const std::byte> byte_view(std::string_view bytes) noexcept {
    return {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
}

// Output budget the stream may still submit before acknowledgements free more.
[[nodiscard]] std::size_t output_budget(http3_session& session, std::int64_t stream_id) noexcept {
    const auto pending = session.codec().pending_output_bytes(stream_id);
    return pending >= http3_stream_output_bytes ? 0 : http3_stream_output_bytes - pending;
}

[[nodiscard]] bool stream_aborted(http3_session& session, std::int64_t stream_id) noexcept {
    const auto* stream = session.find_stream(stream_id);
    return stream == nullptr || stream->aborted();
}

// Waits until acknowledgements free output budget; false when the stream or
// connection ended first.
[[nodiscard]] task<bool> await_output_budget(
    http3_session& session, std::int64_t stream_id, http3_stream_signal& signal) {
    while (output_budget(session, stream_id) == 0) {
        if (signal.stopped() || stream_aborted(session, stream_id)) {
            co_return false;
        }
        signal.set_waiting_output(true);
        co_await signal.wait();
        signal.set_waiting_output(false);
    }
    co_return !signal.stopped() && !stream_aborted(session, stream_id);
}

}  // namespace

task<http3_data_write_status> write_http3_stream_data(
    http3_session& session, std::int64_t stream_id, http3_stream_signal& signal, std::string_view bytes) {
    while (!bytes.empty()) {
        if (signal.terminated()) {
            co_return http3_data_write_status::terminated;
        }
        if (!(co_await await_output_budget(session, stream_id, signal))) {
            co_return signal.terminated() ? http3_data_write_status::terminated
                                          : http3_data_write_status::stream_aborted;
        }
        const auto submitted = session.codec().submit_data(stream_id, byte_view(bytes));
        if (submitted.status_ == http3_status::closed) {
            co_return http3_data_write_status::terminated;
        }
        if (submitted.status_ != http3_status::accepted && submitted.status_ != http3_status::would_block) {
            // The stream no longer accepts content: it was reset.
            co_return http3_data_write_status::stream_aborted;
        }
        bytes.remove_prefix(submitted.accepted_bytes_);
        session.wake_writer();
    }
    co_return http3_data_write_status::completed;
}

http3_buffered_response_writer::http3_buffered_response_writer(
    http3_session& session, response_file_io file_io) noexcept
    : session_(session),
      file_io_(std::move(file_io)),
      file_read_released_(file_io_.worker_) {}

http3_buffered_response_writer::file_read_slot::~file_read_slot() {
    --owner_->file_reads_;
    owner_->file_read_released_.notify();
}

bool http3_buffered_response_writer::stream_ended(std::int64_t stream_id) noexcept {
    auto* stream = session_.find_stream(stream_id);
    if (stream == nullptr || stream->aborted()) {
        return true;
    }
    auto* signal = stream->signal();
    return signal != nullptr && signal->stopped();
}

task<bool> http3_buffered_response_writer::acquire_file_read(std::int64_t stream_id) {
    // Every holder ends its read and submission (or its stream) and notifies,
    // so a waiter wakes to re-check its own stream.
    while (file_reads_ >= http3_file_read_slots) {
        co_await file_read_released_.wait();
        if (stream_ended(stream_id)) {
            co_return false;
        }
    }
    ++file_reads_;
    co_return true;
}

task<http3_buffered_response_write_result> http3_buffered_response_writer::write_file_body(
    std::int64_t stream_id, http3_stream_signal& signal, response_file_source& source,
    http_status_code committed_status) {
    for (;;) {
        std::optional<file_read_slot> slot;
        std::size_t max_bytes = response_file_read_ahead_bytes;
        if (source.file_bytes_pending()) {
            // Read only what the stream may submit: a stream whose peer stopped
            // acknowledging holds neither a read buffer nor a pool task.
            if (!(co_await await_output_budget(session_, stream_id, signal)) ||
                !(co_await acquire_file_read(stream_id))) {
                co_return write_result{.committed_status_ = committed_status};
            }
            slot.emplace(*this);
            if (stream_ended(stream_id)) {
                co_return write_result{.committed_status_ = committed_status};
            }
            const auto budget = output_budget(session_, stream_id);
            if (budget == 0) {
                continue;
            }
            max_bytes = (std::min)(max_bytes, budget);
        }
        const auto chunk = co_await source.next(max_bytes);
        if (chunk.kind() == response_body_chunk::kind_type::failed) {
            // The committed Content-Length can no longer be honoured.
            session_.reset_stream(stream_id, http3_error_code::internal_error);
            co_return write_result{.committed_status_ = committed_status, .file_failure_ = *source.failure()};
        }
        if (chunk.kind() == response_body_chunk::kind_type::end) {
            if (stream_ended(stream_id)) {
                co_return write_result{.committed_status_ = committed_status};
            }
            if (session_.codec().finish_stream(stream_id) != http3_status::accepted) {
                session_.reset_stream(stream_id, http3_error_code::internal_error);
            }
            session_.wake_writer();
            co_return write_result{.committed_status_ = committed_status};
        }
        const auto status = co_await write_http3_stream_data(session_, stream_id, signal, chunk.bytes());
        source.release_buffer();
        if (status != http3_data_write_status::completed) {
            co_return write_result{.committed_status_ = committed_status};
        }
    }
}

task<http3_buffered_response_write_result> http3_buffered_response_writer::write(
    std::int64_t stream_id, const http_response& response, http_buffered_response_write_plan write_plan) {
    if (stream_ended(stream_id)) {
        co_return write_result{};
    }
    std::optional<response_file_source> source;
    // Open and validate the file before the head, without reading it: a file
    // that cannot be served still gets an error response. HEAD and
    // status-suppressed bodies do not transfer a representation.
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
        // The blocking open may have yielded while the peer reset its stream.
        if (stream_ended(stream_id)) {
            co_return write_result{};
        }
        if (!opened) {
            co_return write_result{.file_failure_ = *source->failure()};
        }
    }

    const auto head_result = session_.codec().submit_response_head(stream_id, response, std::move(write_plan));
    const auto* submitted_head = std::get_if<http_buffered_response_write_plan>(&head_result);
    if (submitted_head == nullptr) {
        if (std::get<http3_response_head_error>(head_result) != http3_response_head_error::closed) {
            // Invalid final metadata cannot leave the peer's request waiting
            // for a response the head submission rejected.
            session_.reset_stream(stream_id, http3_error_code::internal_error);
        }
        co_return write_result{};
    }
    auto* stream = session_.find_stream(stream_id);
    auto* signal = stream != nullptr ? stream->signal() : nullptr;
    if (stream == nullptr || signal == nullptr) {
        throw std::logic_error("HTTP/3 response written for an undispatched stream");
    }
    stream->mark_response_committed();
    session_.wake_writer();
    const auto committed_status = submitted_head->body_plan().response_status();
    if (!submitted_head->send_body()) {
        co_return write_result{.committed_status_ = committed_status};
    }
    if (source) {
        co_return co_await write_file_body(stream_id, *signal, *source, committed_status);
    }
    const auto status = co_await write_http3_stream_data(session_, stream_id, *signal, response.body_bytes());
    if (status == http3_data_write_status::completed) {
        if (session_.codec().finish_stream(stream_id) != http3_status::accepted) {
            session_.reset_stream(stream_id, http3_error_code::internal_error);
        }
        session_.wake_writer();
    }
    co_return write_result{.committed_status_ = committed_status};
}

}  // namespace ruvia::detail
