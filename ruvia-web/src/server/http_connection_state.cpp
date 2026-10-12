#include "server/http_connection_state.h"

#include <algorithm>
#include <cstring>
#include <exception>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_pmr_string.h"

#include "server/http1_session_request_completion.h"
#include "server/http_server_options.h"

namespace ruvia::detail {
namespace {

// The read buffer grows up to max_http_header_bytes (see prepare_read); a
// capacity past that means it spilled to hold a body/burst, so reclaim it back
// to the initial size on return. Expressed in terms of the header limit so the
// two move together if that limit is ever retuned.
constexpr std::size_t read_buffer_shrink_capacity_bytes = max_http_header_bytes;
// Upper bound on work sets retained per worker. Beyond this, returned work sets
// release upstream so the free list never grows unbounded after a burst.
constexpr std::size_t max_pooled_work_sets = 64;
// Deferred access-log entries retained across coalesced writes.
constexpr std::size_t retained_deferred_access_entries = 64;

}  // namespace

// The storage is sized to the initial capacity up front so an acquired work
// set is immediately usable; reset() trims it back on return.
connection_read_buffer::connection_read_buffer(std::pmr::polymorphic_allocator<char> allocator)
    : storage_(allocator) {
    http_resize_pmr_string_for_overwrite(storage_, initial_read_buffer_bytes);
}

std::span<char> connection_read_buffer::prepare_read() {
    if (begin_ != 0) {
        // Only the incomplete tail moves, and only before a read: every byte
        // moves at most once between two reads.
        const auto pending_bytes = end_ - begin_;
        if (pending_bytes > 0) {
            std::memmove(storage_.data(), storage_.data() + begin_, pending_bytes);
        }
        begin_ = 0;
        end_ = pending_bytes;
    }
    if (end_ == storage_.size() && storage_.size() < max_http_header_bytes) {
        http_resize_pmr_string_for_overwrite(storage_, std::min(storage_.size() * 2, max_http_header_bytes));
    }
    return std::span<char>(storage_.data() + end_, storage_.size() - end_);
}

void connection_read_buffer::consume(std::size_t bytes) noexcept {
    if (bytes > end_ - begin_) {
        std::terminate();
    }
    begin_ += bytes;
    if (begin_ == end_) {
        begin_ = 0;
        end_ = 0;
    }
}

void connection_read_buffer::assign(std::string_view bytes) {
    if (bytes.size() > storage_.size()) {
        http_resize_pmr_string_for_overwrite(storage_, bytes.size());
    }
    if (!bytes.empty()) {
        std::memcpy(storage_.data(), bytes.data(), bytes.size());
    }
    begin_ = 0;
    end_ = bytes.size();
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
    if (storage_.size() == initial_read_buffer_bytes && storage_.capacity() <= read_buffer_shrink_capacity_bytes) {
        return;
    }
    const auto pending_bytes = end_ - begin_;
    if (pending_bytes > initial_read_buffer_bytes) {
        return;
    }
    if (storage_.capacity() > read_buffer_shrink_capacity_bytes) {
        std::pmr::string compact(storage_.get_allocator());
        http_resize_pmr_string_for_overwrite(compact, initial_read_buffer_bytes);
        if (pending_bytes > 0) {
            std::memcpy(compact.data(), storage_.data() + begin_, pending_bytes);
        }
        storage_ = std::move(compact);
    } else {
        if (begin_ != 0 && pending_bytes > 0) {
            std::memmove(storage_.data(), storage_.data() + begin_, pending_bytes);
        }
        http_resize_pmr_string_for_overwrite(storage_, initial_read_buffer_bytes);
    }
    begin_ = 0;
    end_ = pending_bytes;
}

void connection_read_buffer::reset() {
    begin_ = 0;
    end_ = 0;
    trim();
}

http1_deferred_access_log::http1_deferred_access_log(std::pmr::polymorphic_allocator<char> allocator)
    : entries_(allocator),
      text_(allocator) {}

void http1_deferred_access_log::defer(std::size_t head_end, const http_request& request,
    std::string_view client_address, std::chrono::steady_clock::time_point start, http_status_code status) {
    const auto method = request.method();
    const auto path = request.path();
    const auto text_begin = text_.size();
    text_.append(method);
    text_.append(path);
    text_.append(client_address);
    entries_.push_back({
        .head_end_ = head_end,
        .text_begin_ = text_begin,
        .method_bytes_ = method.size(),
        .path_bytes_ = path.size(),
        .address_bytes_ = client_address.size(),
        .start_ = start,
        .known_method_ = request.known_method(),
        .protocol_version_ = request.protocol_version(),
        .status_ = status,
    });
}

void http1_deferred_access_log::complete(const access_log_sink& access_log, std::size_t written) noexcept {
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
            .remote_address_ = fields.substr(entry.method_bytes_ + entry.path_bytes_, entry.address_bytes_),
            .status_ = entry.status_,
            .duration_micros_ = access_log_record_access::duration_since(entry.start_),
        }));
    }
    reset();
}

void http1_deferred_access_log::reset() noexcept {
    entries_.clear();
    if (entries_.capacity() > retained_deferred_access_entries) {
        std::pmr::vector<entry_type>(entries_.get_allocator()).swap(entries_);
    }
    http_clear_pmr_string_retaining_small(text_);
}

connection_work_set::connection_work_set(worker_memory& memory)
    : read_buffer_(memory.allocator<char>()),
      response_head_(memory.allocator<char>()),
      deferred_access_(memory.allocator<char>()) {}

void connection_work_set::reset_for_reuse() {
    // The read buffer never grows past the 64KB header limit, so this only ever
    // resizes (never rebuilds) and cannot throw in practice; the pool's release
    // still guards against it.
    read_buffer_.reset();
    response_head_.reset();
    deferred_access_.reset();
    // parsed is fully overwritten by the next parse_head(); the parser carries
    // no cross-request state worth clearing.
}

connection_work_set_pool::connection_work_set_pool(worker_memory& memory) noexcept
    : memory_(&memory) {}

connection_work_set_pool::~connection_work_set_pool() {
    auto* current = free_head_;
    while (current != nullptr) {
        auto* next_value = current->pool_next_;
        destroy_pmr_object(current, memory_->resource());
        current = next_value;
    }
}

connection_work_set* connection_work_set_pool::acquire() {
    if (free_head_ != nullptr) {
        auto* work_set = free_head_;
        free_head_ = work_set->pool_next_;
        work_set->pool_next_ = nullptr;
        --free_count_;
        return work_set;
    }
    return construct_pmr_object<connection_work_set>(memory_->resource(), *memory_);
}

void connection_work_set_pool::release(connection_work_set* work_set) noexcept {
    if (work_set == nullptr) {
        return;
    }
    const auto destroy = [this](connection_work_set* victim) noexcept {
        destroy_pmr_object(victim, memory_->resource());
    };
    if (free_count_ >= max_pooled_work_sets) {
        destroy(work_set);
        return;
    }
    try {
        work_set->reset_for_reuse();
    } catch (...) {
        // reset_for_reuse can only throw if the read-buffer rebuild OOMs; drop the
        // work set rather than pooling it, keeping release noexcept.
        destroy(work_set);
        return;
    }
    work_set->pool_next_ = free_head_;
    free_head_ = work_set;
    ++free_count_;
}

}  // namespace ruvia::detail
