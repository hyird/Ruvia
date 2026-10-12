#pragma once
#include <cstddef>

#include <asio/ip/tcp.hpp>

#include "ruvia/core/async.h"

#include "server/http_connection_state.h"

namespace ruvia::detail {

// Bytes of the connection-resident buffer an idle plain-TCP connection reads
// into while it holds no work set. Sized so a typical request line arrives in
// one read; longer heads finish in the regular read loop after the work set
// is re-acquired. It lives in every connection's frame, so growing it to cover
// whole browser heads (400-800 bytes with cookies) costs that much per idle
// connection to save one immediately-completing read per such request.
inline constexpr std::size_t idle_resident_read_bytes = 256;

[[nodiscard]] inline bool plain_tcp_should_wait_for_next_request(const connection_work_set* work_set) noexcept {
    // No available() probe here: FIONREAD costs a syscall per keep-alive
    // request, while a readiness wait on a socket that already has bytes
    // completes inline in the reactor without one. Bytes already parsed into
    // the read buffer are the only state the wait cannot see.
    return work_set == nullptr || work_set->read_buffer_.empty();
}

inline void release_idle_work_set(connection_work_set_pool& pool, connection_work_set*& work_set) {
    if (work_set != nullptr) {
        pool.release(work_set);
        work_set = nullptr;
    }
}

}  // namespace ruvia::detail
