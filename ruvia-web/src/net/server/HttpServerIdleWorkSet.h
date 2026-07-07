#pragma once

#include "ConnectionScanner.h"
#include "HttpConnectionState.h"
#include "runtime/AsioAwait.h"

#include <asio/ip/tcp.hpp>
#include <atomic>
#include <cstddef>
#include <system_error>
#include <utility>

namespace ruvia::detail {

[[nodiscard]] inline bool plainTcpShouldWaitForNextRequest(
    asio::ip::tcp::socket& socket,
    std::size_t usedBytes) {
    if (usedBytes != 0) {
        return false;
    }
    std::error_code availabilityEc;
    const auto pendingBytes = socket.available(availabilityEc);
    return !availabilityEc && pendingBytes == 0;
}

inline void releaseIdleWorkSet(
    ConnectionWorkSetPool& pool,
    ConnectionWorkSet*& workSet) {
    if (workSet != nullptr) {
        pool.release(workSet);
        workSet = nullptr;
    }
}

inline Task<std::error_code> waitForPlainTcpReadable(
    asio::ip::tcp::socket& socket,
    ConnectionScanner::Entry& scannerEntry) {
    scannerEntry.setPhase(ConnectionScanner::Phase::kReadingHeader);
    co_return co_await asyncError([&socket](auto handler) mutable {
        socket.async_wait(asio::ip::tcp::socket::wait_read, std::move(handler));
    });
}

}  // namespace ruvia::detail
