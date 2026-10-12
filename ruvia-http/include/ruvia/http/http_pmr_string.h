#pragma once

#include <cstddef>
#include <memory_resource>
#include <string>

namespace ruvia {

// Retention table: the most storage a reusable ruvia-http buffer keeps once it
// is drained. Anything larger goes back to its owner's resource, so one large
// message or head never stays pinned to an idle connection or worker, while
// buffers within the bound are reused without reallocating.

// Worker-local temporary strings and buffers (heads, client write buffers).
inline constexpr std::size_t http_retained_pmr_string_bytes = 4096;

// Each WebSocket connection buffer: transport input, pending output,
// compressed outbound payload and decompressed inbound message.
inline constexpr std::size_t websocket_retained_buffer_bytes = std::size_t{16} * 1024;

// Transport read that waits for the next frame on a server WebSocket
// connection whose recent frames were small. It bounds the read storage an
// idle connection holds; a larger frame grows the buffer on demand.
inline constexpr std::size_t websocket_idle_read_bytes = 512;

// Clear a temporary buffer, returning unusually large storage to its owner
// while retaining small allocations for repeated worker-local operations.
inline void http_clear_pmr_string_retaining_small(
    std::pmr::string& target, std::size_t retained_bytes = http_retained_pmr_string_bytes) {
    target.clear();
    if (target.capacity() <= retained_bytes) {
        return;
    }
    std::pmr::string empty(target.get_allocator());
    target.swap(empty);
}

}  // namespace ruvia
