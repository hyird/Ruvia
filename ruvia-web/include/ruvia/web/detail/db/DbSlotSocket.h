#pragma once

#include <asio/posix/stream_descriptor.hpp>

namespace ruvia::detail {

// Persistent ASIO wrapper around a database driver's native connection socket.
//
// MariaDB and libpq retain ownership while ASIO waits for readiness, so the
// wrapper is detached without closing the native descriptor.
struct DbSlotSocket final {
    explicit DbSlotSocket(asio::io_context& ioContext);

    using NativeSocket = asio::posix::stream_descriptor::native_handle_type;
    asio::posix::stream_descriptor descriptor;
    static constexpr NativeSocket kInvalidSocket = -1;
    NativeSocket native{kInvalidSocket};

    [[nodiscard]] bool ensureAssigned(NativeSocket fd) noexcept;
    void cancel() noexcept;
    // Detaches Asio without closing the driver-owned native descriptor.
    // Failure cannot be ignored: destroying an attached wrapper would close a
    // handle that MariaDB/libpq still owns.
    [[nodiscard]] bool release() noexcept;
};

}  // namespace ruvia::detail
