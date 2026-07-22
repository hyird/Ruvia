#pragma once
#include "ruvia/web/detail/server/session/HttpConnectionState.h"
#include "ruvia/core/detail/io/AsioAwait.h"

#include <asio/ip/tcp.hpp>
#include <atomic>
#include <cstddef>
#include <system_error>
#include <utility>

namespace ruvia::detail {

// Bytes of the connection-resident buffer an idle plain-TCP connection reads
// into while it holds no work set. Sized so a typical request line arrives in
// one read; longer heads finish in the regular read loop after the work set
// is re-acquired.
inline constexpr std::size_t kIdleResidentReadBytes = 256;

[[nodiscard]] inline bool plainTcpShouldWaitForNextRequest(
    std::size_t usedBytes) noexcept {
    // No available() probe here: FIONREAD costs a syscall per keep-alive
    // request, while a readiness wait on a socket that already has bytes
    // completes inline in the reactor without one. Bytes already parsed into
    // the read buffer are the only state the wait cannot see.
    return usedBytes == 0;
}

inline void releaseIdleWorkSet(
    ConnectionWorkSetPool& pool,
    ConnectionWorkSet*& workSet) {
    if (workSet != nullptr) {
        pool.release(workSet);
        workSet = nullptr;
    }
}

}  // namespace ruvia::detail
