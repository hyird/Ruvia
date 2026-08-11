#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <asio/ip/tcp.hpp>
#else
#include <asio/posix/stream_descriptor.hpp>
#endif

#include <cstdint>
#include <limits>
#include <system_error>

namespace ruvia::detail {

// Transient ASIO wrapper around a database driver's native connection socket.
//
// MariaDB and libpq retain ownership of their native socket. ASIO waits on a
// duplicate owned solely by this wrapper, so cleanup never depends on
// basic_socket::release(), whose Windows IOCP cleanup can fail after cancellation
// or after the driver has already closed its handle, and can never create two
// owners of the driver's handle.
struct DbSlotSocket final {
    explicit DbSlotSocket(asio::io_context& ioContext);

#if defined(_WIN32)
    using NativeSocket = std::uintptr_t;
    asio::ip::tcp::socket socket;
    static constexpr NativeSocket kInvalidSocket = std::numeric_limits<NativeSocket>::max();
#else
    using NativeSocket = asio::posix::stream_descriptor::native_handle_type;
    asio::posix::stream_descriptor descriptor;
    static constexpr NativeSocket kInvalidSocket = -1;
#endif
    NativeSocket native{kInvalidSocket};

    [[nodiscard]] std::error_code ensureAssigned(NativeSocket fd) noexcept;
    // Puts the driver-owned descriptor into non-blocking mode. A driver whose
    // asynchronous API suspends on EAGAIN cannot suspend at all while its
    // socket blocks, so it runs the whole operation inside the call that was
    // supposed to start it.
    [[nodiscard]] std::error_code makeNonBlocking() noexcept;
    void cancel() noexcept;
    // Closes only the ASIO-owned duplicate. The driver's original socket stays
    // valid until mysql_close()/PQfinish() disposes it.
    void reset() noexcept;
};

}  // namespace ruvia::detail
