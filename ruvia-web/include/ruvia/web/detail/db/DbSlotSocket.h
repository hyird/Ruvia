#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <asio/ip/tcp.hpp>
#else
#include <asio/posix/stream_descriptor.hpp>
#endif

#include <cstdint>
#include <limits>

namespace ruvia::detail {

// Persistent ASIO wrapper around a database driver's native connection socket.
//
// MariaDB and libpq retain ownership while ASIO waits for readiness. On Windows
// the IOCP backend permanently associates a handle with the completion port,
// so the wrapper is assigned once per native socket and reused across waits.
struct DbSlotSocket final {
    explicit DbSlotSocket(asio::io_context& ioContext);

#if defined(_WIN32)
    using NativeSocket = std::uintptr_t;
    asio::ip::tcp::socket socket;
    static constexpr NativeSocket kInvalidSocket =
        std::numeric_limits<NativeSocket>::max();
#else
    using NativeSocket = asio::posix::stream_descriptor::native_handle_type;
    asio::posix::stream_descriptor descriptor;
    static constexpr NativeSocket kInvalidSocket = -1;
#endif
    NativeSocket native{kInvalidSocket};

    [[nodiscard]] bool ensureAssigned(NativeSocket fd) noexcept;
    void cancel() noexcept;
    void release() noexcept;
};

}  // namespace ruvia::detail
