#include "server/http_connection_state.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/pmr_string.h"
#include "ruvia/http/http_limits.h"

#include "server/http1_session_request_completion.h"

namespace ruvia::detail {
namespace {

// The read buffer grows up to max_http_header_bytes (see grow_read_buffer); a
// capacity past that means it spilled to hold a body/burst, so reclaim it back
// to the initial size on return. Expressed in terms of the header limit so the
// two move together if that limit is ever retuned.
constexpr std::size_t read_buffer_shrink_capacity_bytes = max_http_header_bytes;
// Upper bound on work sets retained per worker. Beyond this, returned work sets
// release upstream so the free list never grows unbounded after a burst.
constexpr std::size_t max_pooled_work_sets = 64;

}  // namespace

// The read buffer is sized to the initial capacity up front so an acquired work
// set is immediately usable; reset_for_reuse() trims it back on return.
connection_work_set::connection_work_set(worker_memory& memory)
    : read_buffer_(memory.allocator<char>()),
      response_head_(memory.allocator<char>()),
      file_chunk_(memory.allocator<char>()) {
    ::ruvia::resize_pmr_string_for_overwrite(read_buffer_, initial_read_buffer_bytes);
}

void connection_work_set::reset_for_reuse() {
    // The read buffer never grows past the 64KB header limit, so this only ever
    // resizes (never rebuilds) and cannot throw in practice; the pool's release
    // still guards against it.
    trim_read_buffer_storage(read_buffer_, 0);
    response_head_.reset();
    // parsed is fully overwritten by the next parse_head(); file_chunk/parser
    // carry no cross-request state worth clearing.
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

void compact_connection_read_buffer(
    std::pmr::string& read_buffer, std::size_t& used_bytes, std::size_t consumed_bytes) noexcept {
    const auto remaining_bytes = used_bytes - consumed_bytes;
    if (remaining_bytes > 0) {
        std::memmove(read_buffer.data(), read_buffer.data() + consumed_bytes, remaining_bytes);
    }
    used_bytes = remaining_bytes;
}

void install_connection_read_buffer_pipeline(
    std::pmr::string& read_buffer, std::size_t& used_bytes, std::string_view pipeline) {
    // `pipeline` is request-scoped storage handed over by a body runtime, never
    // an alias of read_buffer, so this copies rather than shifts in place.
    if (pipeline.size() > read_buffer.size()) {
        ::ruvia::resize_pmr_string_for_overwrite(read_buffer, pipeline.size());
    }
    if (!pipeline.empty()) {
        std::memcpy(read_buffer.data(), pipeline.data(), pipeline.size());
    }
    used_bytes = pipeline.size();
}

void apply_reusable_http1_request_buffer_completion(const http1_request_buffer_completion& completion,
    std::pmr::string& read_buffer, std::size_t& used_bytes) {
    if (const auto* compaction = completion.compaction()) {
        if (compaction->consumed_bytes() > used_bytes) {
            std::terminate();
        }
        compact_connection_read_buffer(read_buffer, used_bytes, compaction->consumed_bytes());
        return;
    }
    if (const auto* restore = completion.pipeline_restore()) {
        install_connection_read_buffer_pipeline(read_buffer, used_bytes, restore->pipeline());
        return;
    }
    // A discarded buffer is valid only when the connection plan closes. The
    // session must never reach reusable cleanup with that alternative.
    std::terminate();
}

void trim_read_buffer_storage(std::pmr::string& read_buffer, std::size_t used_bytes) {
    if (used_bytes > initial_read_buffer_bytes) {
        return;
    }

    if (read_buffer.capacity() > read_buffer_shrink_capacity_bytes) {
        std::pmr::string compact(read_buffer.get_allocator());
        ::ruvia::resize_pmr_string_for_overwrite(compact, initial_read_buffer_bytes);
        if (used_bytes > 0) {
            std::memcpy(compact.data(), read_buffer.data(), used_bytes);
        }
        read_buffer = std::move(compact);
        return;
    }

    if (read_buffer.size() != initial_read_buffer_bytes) {
        ::ruvia::resize_pmr_string_for_overwrite(read_buffer, initial_read_buffer_bytes);
    }
}

void grow_read_buffer(std::pmr::string& read_buffer, std::size_t used_bytes) {
    if (used_bytes == read_buffer.size() && read_buffer.size() < max_http_header_bytes) {
        ::ruvia::resize_pmr_string_for_overwrite(
            read_buffer, std::min(read_buffer.size() * 2, max_http_header_bytes));
    }
}

}  // namespace ruvia::detail
