#pragma once
#include <atomic>
#include <cstddef>
#include <system_error>
#include <utility>

#include <asio/ip/tcp.hpp>

#include "ruvia/core/async.h"

#include "server/http_connection_state.h"

namespace ruvia::detail {

// Bytes of the connection-resident buffer an idle plain-TCP connection reads
// into while it holds no work set. Sized so a typical request line arrives in
// one read; longer heads finish in the regular read loop after the work set
// is re-acquired.
inline constexpr std::size_t idle_resident_read_bytes = 256;

[[nodiscard]] inline bool plain_tcp_should_wait_for_next_request(std::size_t used_bytes) noexcept {
    // No available() probe here: FIONREAD costs a syscall per keep-alive
    // request, while a readiness wait on a socket that already has bytes
    // completes inline in the reactor without one. Bytes already parsed into
    // the read buffer are the only state the wait cannot see.
    return used_bytes == 0;
}

inline void release_idle_work_set(connection_work_set_pool& pool, connection_work_set*& work_set) {
    if (work_set != nullptr) {
        pool.release(work_set);
        work_set = nullptr;
    }
}

}  // namespace ruvia::detail
