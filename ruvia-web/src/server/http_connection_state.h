#pragma once

#include <chrono>
#include <cstddef>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response_head_buffer.h"
#include "ruvia/http/http_status.h"

#include "router/route_resolution.h"
#include "server/connection_scanner.h"

namespace ruvia::detail {

class http1_request_buffer_completion;
struct access_log_sink;

// Initial bump block for the per-request arena, carried inside a work set. The
// request monotonic_buffer_resource bump-allocates from here before spilling to
// the worker resource, so a typical small request touches no heap at all. Sized
// to request_arena_initial_bytes (see memory_pool_config.h).
inline constexpr std::size_t work_set_arena_bytes = request_arena_initial_bytes;

// Fresh size of a work set's read buffer; reset() restores it before the work
// set is pooled, so a borrower always starts from this capacity. 4 KB holds a
// typical request head with room to spare, and prepare_read() doubles on demand
// up to max_http_header_bytes; keeping the fresh size small matters because
// under high concurrency every active connection holds one.
inline constexpr std::size_t initial_read_buffer_bytes = std::size_t{4} * 1024;

// A connection's buffered request input: storage plus the unconsumed window.
// A served request is consumed by advancing the window start; the window moves
// to the front only when a read needs room. A burst of pipelined requests
// therefore costs one move of its incomplete tail per socket read, not one
// shift of every remaining byte per request.
class connection_read_buffer final {
public:
    explicit connection_read_buffer(std::pmr::polymorphic_allocator<char> allocator);

    // Unconsumed bytes, starting with the current request.
    [[nodiscard]] std::string_view pending() const noexcept {
        return std::string_view(storage_.data() + begin_, end_ - begin_);
    }
    [[nodiscard]] bool empty() const noexcept {
        return begin_ == end_;
    }

    // Room for the next socket read. Moves the window to the front and, when
    // it fills the storage, doubles the storage up to max_http_header_bytes.
    // Empty when the pending bytes already fill that limit.
    [[nodiscard]] std::span<char> prepare_read();
    // Appends `bytes` read into the span prepare_read() returned.
    void commit_read(std::size_t bytes) noexcept {
        end_ += bytes;
    }

    // Drops the first `bytes` pending bytes (a served request).
    void consume(std::size_t bytes) noexcept;
    // Replaces the pending bytes with a copy of `bytes`, which never aliases
    // this buffer (an idle landing pad or a body runtime's over-read stash).
    void assign(std::string_view bytes);
    // Applies a reusable request's buffer completion. Installing a handed-over
    // pipeline can grow the storage, so this is the one completion step that
    // may allocate.
    void complete_request(const http1_request_buffer_completion& completion);

    // Gives grown storage back once the pending bytes fit the initial size.
    void trim();
    // Empties the buffer and trims it for the next borrower.
    void reset();

private:
    std::pmr::string storage_;
    std::size_t begin_{0};
    std::size_t end_{0};
};

// Access-log fields of pipelined responses held back for a coalesced write.
// The request storage they come from is reused before that write, so the
// logged fields are copied; nothing is kept without an access log.
class http1_deferred_access_log final {
public:
    explicit http1_deferred_access_log(std::pmr::polymorphic_allocator<char> allocator);

    // Keeps the record of a response whose head completes at output offset
    // `head_end`. Duration is measured when the record is reported.
    void defer(std::size_t head_end, const http_request& request, std::string_view client_address,
        std::chrono::steady_clock::time_point start, http_status_code status);
    // Reports every kept response whose head lies within the first `written`
    // output bytes (in output order), then forgets all of them.
    void complete(const access_log_sink& access_log, std::size_t written) noexcept;
    // Forgets every kept response and releases grown storage.
    void reset() noexcept;

private:
    struct entry_type final {
        std::size_t head_end_;
        std::size_t text_begin_;
        std::size_t method_bytes_;
        std::size_t path_bytes_;
        std::size_t address_bytes_;
        std::chrono::steady_clock::time_point start_;
        http_known_method known_method_;
        http_protocol_version protocol_version_;
        http_status_code status_;
    };

    std::pmr::vector<entry_type> entries_;
    std::pmr::string text_;
};

// All of a connection's heavy per-request working memory bundled into one
// poolable unit: the read buffer, the request arena block, the (reused) parse
// result and the response output buffer. A connection borrows a work set only
// while it is actively serving requests and returns it the moment it goes
// idle, so idle keep-alive connections hold none of it and memory scales with
// in-flight request concurrency rather than connection count. The
// connection's small resident identity (socket, scanner entry, keep-alive
// counters) stays in the session coroutine frame.
struct connection_work_set final {
    explicit connection_work_set(worker_memory& memory);

    connection_read_buffer read_buffer_;
    // Serialized response output not yet written: the current response's head
    // (and coalesced small body), preceded by any pipelined responses held
    // back for one write. Empty between writes.
    http_response_head_buffer response_head_;
    http1_deferred_access_log deferred_access_;
    http1_server_request_parser parser_;
    http1_server_request_parse_state parsed_;
    route_resolution route_resolution_;
    alignas(std::max_align_t) std::byte arena_block_[work_set_arena_bytes];
    connection_work_set* pool_next_{nullptr};

    // Return the work set to a clean borrowable state: trim grown read storage
    // back to the initial size and clear per-request scratch.
    void reset_for_reuse();
};

// Per-worker intrusive free list of work sets. Worker-private and only touched
// from the worker thread (single-threaded cooperative coroutines), so it needs
// no synchronization. Borrow = pop (warm, no allocation); return = reset + push,
// or free to the upstream resource once the cap is reached.
class connection_work_set_pool final {
public:
    explicit connection_work_set_pool(worker_memory& memory) noexcept;
    ~connection_work_set_pool();

    connection_work_set_pool(const connection_work_set_pool&) = delete;
    connection_work_set_pool& operator=(const connection_work_set_pool&) = delete;

    [[nodiscard]] connection_work_set* acquire();
    void release(connection_work_set* work_set) noexcept;

private:
    worker_memory* memory_;
    connection_work_set* free_head_{nullptr};
    std::size_t free_count_{0};
};

}  // namespace ruvia::detail
