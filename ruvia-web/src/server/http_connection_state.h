#pragma once

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/core/connection_scanner.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http_response_head_buffer.h"

#include "router/route_resolution.h"

namespace ruvia::detail {

class http1_request_buffer_completion;

// Initial bump block for the per-request arena, carried inside a work set. The
// request monotonic_buffer_resource bump-allocates from here before spilling to
// the worker resource, so a typical small request touches no heap at all. Sized
// to the shared request_arena_initial_bytes (see memory_pool_config.h) so the HTTP/1 and
// HTTP/2 request arenas start from one identical block size.
inline constexpr std::size_t work_set_arena_bytes = request_arena_initial_bytes;

// Fresh size of a work set's read buffer; reset_for_reuse() restores it before
// the work set is pooled, so a borrower always starts from this capacity.
// 4 KB holds a typical request head with room to spare, and grow_read_buffer()
// doubles on demand up to max_http_header_bytes; keeping the fresh size small
// matters because under high concurrency every active connection holds one.
inline constexpr std::size_t initial_read_buffer_bytes = std::size_t{4} * 1024;

// All of a connection's heavy per-request working memory bundled into one
// poolable unit: the read buffer, the request arena block, the (reused) parse
// result, the response-head buffer, and the file chunk buffer. A connection
// borrows a work set only while it is actively serving requests and returns it
// the moment it goes idle, so idle keep-alive connections hold none of it and
// memory scales with in-flight request concurrency rather than connection
// count. The connection's small resident identity (socket, scanner entry,
// keep-alive counters) stays in the session coroutine frame.
struct connection_work_set final {
    explicit connection_work_set(worker_memory& memory);

    std::pmr::string read_buffer_;
    http_response_head_buffer response_head_;
    std::pmr::string file_chunk_;
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

void compact_connection_read_buffer(
    std::pmr::string& read_buffer, std::size_t& used_bytes, std::size_t consumed_bytes) noexcept;
void install_connection_read_buffer_pipeline(
    std::pmr::string& read_buffer, std::size_t& used_bytes, std::string_view pipeline);
// Installing a handed-over pipeline can grow the read buffer, so this is the one
// buffer-completion step that may allocate.
void apply_reusable_http1_request_buffer_completion(const http1_request_buffer_completion& completion,
    std::pmr::string& read_buffer, std::size_t& used_bytes);
void trim_read_buffer_storage(std::pmr::string& read_buffer, std::size_t used_bytes);
void grow_read_buffer(std::pmr::string& read_buffer, std::size_t used_bytes);

}  // namespace ruvia::detail
