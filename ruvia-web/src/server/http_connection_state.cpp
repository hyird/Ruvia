#include "server/http_connection_state.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <new>

#include "ruvia/http/http_limits.h"

#include "server/http1_session_request_completion.h"
#include "server/http_server_options.h"

namespace ruvia::detail {
namespace {

// Reclaim read storage beyond the header limit once the pending bytes fit the
// initial size. Amortized growth and handed-over pipelines can both exceed it.
constexpr std::size_t read_buffer_shrink_capacity_bytes = max_http_header_bytes;
// Deferred access-log entries retained across coalesced writes.
constexpr std::size_t retained_deferred_access_entries = 64;

}  // namespace

// Reserve the initial capacity before exposing the active work set.
connection_read_buffer::connection_read_buffer() noexcept {
    storage_.reserve(initial_read_buffer_bytes);
}

std::span<char> connection_read_buffer::prepare_read() & {
    if (storage_.size() >= max_http_header_bytes) {
        return {};
    }
    const auto remaining = max_http_header_bytes - storage_.size();
    auto space = storage_.prepare_spare();
    if (space.empty()) {
        // Storage may outgrow the protocol window to amortize a short consumed
        // prefix; only the bytes still allowed in that window are writable.
        (void)storage_.prepare(std::min(initial_read_buffer_bytes, remaining));
        space = storage_.prepare_spare();
    }
    return space.first(std::min(space.size(), remaining));
}

void connection_read_buffer::consume(std::size_t bytes) noexcept {
    if (bytes > storage_.size()) {
        std::terminate();
    }
    storage_.consume(bytes);
}

void connection_read_buffer::assign(std::string_view bytes) {
    storage_.clear();
    storage_.append(bytes);
}

void connection_read_buffer::complete_request(const http1_request_buffer_completion& completion) {
    if (const auto* compaction = completion.compaction()) {
        consume(compaction->consumed_bytes());
        return;
    }
    if (const auto* restore = completion.pipeline_restore()) {
        assign(restore->pipeline());
        return;
    }
    // A discarded buffer is valid only when the connection plan closes. The
    // session must never reach reusable cleanup with that alternative.
    std::terminate();
}

void connection_read_buffer::trim() {
    if (storage_.capacity() > read_buffer_shrink_capacity_bytes && storage_.size() <= initial_read_buffer_bytes) {
        storage_.shrink_to(initial_read_buffer_bytes);
    }
}

http1_deferred_access_log::http1_deferred_access_log() noexcept = default;

void http1_deferred_access_log::defer(std::size_t head_end, const http_request& request,
    ip_address client_address, std::chrono::steady_clock::time_point start,
    http_status_code status, const request_io_metrics& io_metrics) {
    const auto method = request.method();
    const auto path = request.path();
    const auto text_begin = text_.size();
    const auto required_text = text_begin + method.size() + path.size();
    if (required_text > text_.capacity()) {
        text_.reserve(
            std::max<std::size_t>(required_text, std::max<std::size_t>(256, text_.capacity() * 2)));
    }
    text_.append(method);
    text_.append(path);
    if (entries_.size() == entries_.capacity()) {
        entries_.reserve(std::max<std::size_t>(8, entries_.capacity() * 2));
    }
    entries_.push_back({
        .head_end_ = head_end,
        .text_begin_ = text_begin,
        .method_bytes_ = method.size(),
        .path_bytes_ = path.size(),
        .client_address_ = client_address,
        .start_ = start,
        .io_metrics_ = io_metrics,
        .known_method_ = request.known_method(),
        .protocol_version_ = request.protocol_version(),
        .status_ = status,
    });
}

void http1_deferred_access_log::complete(
    const access_log_sink& access_log, ip_address remote_address, std::size_t written) noexcept {
    const std::string_view text = text_;
    for (const auto& entry : entries_) {
        if (entry.head_end_ > written) {
            break;
        }
        const auto fields = text.substr(entry.text_begin_);
        access_log.invoke(access_log_record_access::make({
            .method_ = fields.substr(0, entry.method_bytes_),
            .known_method_ = entry.known_method_,
            .path_ = fields.substr(entry.method_bytes_, entry.path_bytes_),
            .protocol_version_ = entry.protocol_version_,
            .remote_address_ = remote_address,
            .client_address_ = entry.client_address_,
            .status_ = entry.status_,
            .duration_micros_ = access_log_record_access::duration_since(entry.start_),
            .db_count_ = entry.io_metrics_.db_count_,
            .db_duration_micros_ = entry.io_metrics_.db_duration_micros_,
            .redis_count_ = entry.io_metrics_.redis_count_,
            .redis_duration_micros_ = entry.io_metrics_.redis_duration_micros_,
        }));
    }
    reset();
}

void http1_deferred_access_log::reset() noexcept {
    entries_.clear();
    if (entries_.capacity() > retained_deferred_access_entries) {
        std::vector<entry_type>(entries_.get_allocator()).swap(entries_);
    }
    http_clear_string_retaining_small(text_);
}

connection_work_set::connection_work_set() noexcept = default;

std::unique_ptr<connection_work_set> make_connection_work_set() {
    return std::make_unique<connection_work_set>();
}

}  // namespace ruvia::detail
