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
#include "ruvia/http/http_io_buffer.h"
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

// Fresh size of a work set's read buffer; reset() restores it before the work
// set is pooled, so a borrower always starts from this capacity and the pool
// retains at most this much read storage per work set. 4 KB holds a typical
// request head with room to spare, and prepare_read() doubles on demand up to
// max_http_header_bytes; keeping the fresh size small matters because under
// high concurrency every active connection holds one.
inline constexpr std::size_t initial_read_buffer_bytes = std::size_t{4} * 1024;

// Bytes of the connection-resident buffer an idle connection reads into while
// it holds no work set. Sized so a typical request line arrives in one read;
// longer heads finish in the regular read loop after the work set is
// re-acquired. It lives in every connection's frame, so growing it to cover
// whole browser heads (400-800 bytes with cookies) costs that much per idle
// connection to save one immediately-completing read per such request.
inline constexpr std::size_t idle_resident_read_bytes = 256;

// A connection's buffered request input: an http_io_buffer plus its growth and
// trim policy. A served request is consumed by advancing the window start; the
// window moves to the front only when a read needs room. A burst of pipelined
// requests therefore costs one move of its incomplete tail per socket read,
// not one shift of every remaining byte per request.
class connection_read_buffer final {
public:
    explicit connection_read_buffer(std::pmr::polymorphic_allocator<char> allocator);

    // Unconsumed bytes, starting with the current request.
    [[nodiscard]] std::string_view pending() const noexcept {
        return storage_.view();
    }
    [[nodiscard]] bool empty() const noexcept {
        return storage_.empty();
    }

    // Room for the next socket read: all spare storage. When the pending bytes
    // fill the storage, doubles it up to max_http_header_bytes. Empty when the
    // pending bytes already fill that limit.
    [[nodiscard]] std::span<char> prepare_read();
    // Appends `bytes` read into the span prepare_read() returned.
    void commit_read(std::size_t bytes) noexcept {
        storage_.commit(bytes);
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

    // Gives storage that a handed-over pipeline grew past max_http_header_bytes
    // back once the pending bytes fit the initial size.
    void trim();
    // Empties the buffer for the next borrower. Storage that grew past the
    // initial size is replaced with a fresh initial-capacity block, so the
    // grown block goes back to the worker resource.
    void reset();

private:
    http_io_buffer storage_;
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
//
// The pool allocates a work set and its request arena block together; the
// block follows the work set and is the worker's configured
// request_initial_buffer_bytes() long, so a typical request bump-allocates
// from it and touches no heap at all.
struct connection_work_set final {
    connection_work_set(worker_memory& memory, std::span<std::byte> arena_block);

    connection_read_buffer read_buffer_;
    // Serialized response output not yet written: the current response's head
    // (and coalesced small body), preceded by any pipelined responses held
    // back for one write. Empty between writes.
    http_response_head_buffer response_head_;
    http1_deferred_access_log deferred_access_;
    http1_server_request_parser parser_;
    http1_server_request_parse_state parsed_;
    route_resolution route_resolution_;
    // Initial block of the per-request arena (see above).
    std::span<std::byte> arena_block_;
    connection_work_set* pool_next_{nullptr};

    // Return the work set to a clean borrowable state: replace grown read
    // storage with an initial-size block and clear per-request scratch.
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
    void destroy(connection_work_set* work_set) noexcept;

    worker_memory* memory_;
    // One allocation: the work set followed by its arena block.
    std::size_t arena_bytes_;
    std::size_t allocation_bytes_;
    connection_work_set* free_head_{nullptr};
    std::size_t free_count_{0};
};

}  // namespace ruvia::detail
