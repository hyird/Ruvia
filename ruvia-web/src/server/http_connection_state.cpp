#include "server/http_connection_state.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <new>

#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_pmr_string.h"

#include "server/http1_session_request_completion.h"
#include "server/http_server_options.h"

namespace ruvia::detail {
namespace {

// Capacity past which a handed-over pipeline grew the read buffer beyond what
// prepare_read() ever reaches; trim() reclaims it mid-connection once the
// pending bytes fit the initial size. Expressed in terms of the header limit
// so the two move together if that limit is ever retuned.
constexpr std::size_t read_buffer_shrink_capacity_bytes = max_http_header_bytes;
// Capacity a pooled work set keeps across borrowers. A head of 4-8 KB
// (cookie-heavy browser requests) would otherwise grow the buffer on every
// request and have reset() shrink it back on every idle return: two
// reallocations plus a copy per keep-alive request for the connection's
// whole life. Growth up to this ceiling is retained; only larger storage is
// given back, so the pool holds at most this much read storage per work set.
constexpr std::size_t retained_read_buffer_bytes = std::min(std::size_t{16} * 1024, max_http_header_bytes);
// Upper bound on work sets retained per worker. Beyond this, returned work sets
// release upstream so the free list never grows unbounded after a burst.
constexpr std::size_t max_pooled_work_sets = 64;
// Deferred access-log entries retained across coalesced writes.
constexpr std::size_t retained_deferred_access_entries = 64;

}  // namespace

// The storage is reserved to the initial capacity up front so an acquired work
// set is immediately usable; reset() trims it back on return.
connection_read_buffer::connection_read_buffer(std::pmr::polymorphic_allocator<char> allocator)
    : storage_(allocator) {
    storage_.reserve(initial_read_buffer_bytes);
}

std::span<char> connection_read_buffer::prepare_read() {
    if (storage_.size() == storage_.capacity() && storage_.capacity() < max_http_header_bytes) {
        storage_.reserve(std::min(std::max(storage_.capacity() * 2, initial_read_buffer_bytes), max_http_header_bytes));
    }
    // Reading into the spare storage at most moves the incomplete tail to the
    // front: every byte moves at most once per read, and only the doubling
    // above grows the storage however pipelined input is split.
    return storage_.prepare_spare();
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

void connection_read_buffer::reset() {
    storage_.clear();
    // With no pending bytes, shrinking reallocates: the grown block is freed
    // and an initial-size one takes its place. Growth within the retained
    // ceiling stays, so the next borrower's head of that size reads into it
    // without growing again.
    if (storage_.capacity() > retained_read_buffer_bytes) {
        storage_.shrink_to(initial_read_buffer_bytes);
    }
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

connection_work_set::connection_work_set(worker_memory& memory, std::span<std::byte> arena_block)
    : read_buffer_(memory.allocator<char>()),
      response_head_(memory.allocator<char>()),
      deferred_access_(memory.allocator<char>()),
      arena_block_(arena_block) {}

void connection_work_set::reset_for_reuse() {
    // Replacing a grown read buffer allocates its fresh block, so this can
    // throw bad_alloc; the pool's release drops the work set then.
    read_buffer_.reset();
    response_head_.reset();
    deferred_access_.reset();
    // parsed is fully overwritten by the next parse_head(); the parser carries
    // no cross-request state worth clearing.
}

// A configured arena block too large to follow a work set saturates the
// allocation size, which the upstream then refuses with bad_alloc.
connection_work_set_pool::connection_work_set_pool(worker_memory& memory) noexcept
    : memory_(&memory),
      arena_bytes_(memory.request_initial_buffer_bytes()),
      allocation_bytes_(arena_bytes_ > std::numeric_limits<std::size_t>::max() - sizeof(connection_work_set)
                            ? std::numeric_limits<std::size_t>::max()
                            : sizeof(connection_work_set) + arena_bytes_) {}

connection_work_set_pool::~connection_work_set_pool() {
    auto* current = free_head_;
    while (current != nullptr) {
        auto* next_value = current->pool_next_;
        destroy(current);
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
    auto* const resource = memory_->resource();
    auto* const storage = static_cast<std::byte*>(resource->allocate(allocation_bytes_, alignof(connection_work_set)));
    try {
        return ::new (static_cast<void*>(storage))
            connection_work_set(*memory_, std::span<std::byte>(storage + sizeof(connection_work_set), arena_bytes_));
    } catch (...) {
        resource->deallocate(storage, allocation_bytes_, alignof(connection_work_set));
        throw;
    }
}

void connection_work_set_pool::release(connection_work_set* work_set) noexcept {
    if (work_set == nullptr) {
        return;
    }
    if (free_count_ >= max_pooled_work_sets) {
        destroy(work_set);
        return;
    }
    try {
        work_set->reset_for_reuse();
    } catch (...) {
        // Only replacing a grown read buffer can throw (bad_alloc); drop the
        // work set rather than pooling it, keeping release noexcept.
        destroy(work_set);
        return;
    }
    work_set->pool_next_ = free_head_;
    free_head_ = work_set;
    ++free_count_;
}

void connection_work_set_pool::destroy(connection_work_set* work_set) noexcept {
    work_set->~connection_work_set();
    memory_->resource()->deallocate(work_set, allocation_bytes_, alignof(connection_work_set));
}

}  // namespace ruvia::detail
