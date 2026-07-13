#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <asio/ip/tcp.hpp>
#else
#include <asio/posix/stream_descriptor.hpp>
#endif
#include <mysql/mysql.h>

namespace ruvia::detail {

// Persistent ASIO wrapper around a single MariaDB connection socket.
//
// MariaDB hands us a native socket via mysql_get_socket(); we need ASIO to wait
// for readiness on it. On Windows the IOCP backend permanently associates a
// socket handle with the completion port when assign() is called, and release()
// cannot undo that association. Re-assigning the same fd on every wait therefore
// fails the second time (ERROR_INVALID_PARAMETER) and breaks the connection.
// To stay portable we assign the socket exactly once per connection here and
// reuse it for every subsequent wait.
struct SlotSocket final {
    explicit SlotSocket(asio::io_context& ioContext);

#if defined(_WIN32)
    asio::ip::tcp::socket socket;
#else
    asio::posix::stream_descriptor descriptor;
#endif
    my_socket native{static_cast<my_socket>(MARIADB_INVALID_SOCKET)};

    [[nodiscard]] bool ensureAssigned(my_socket fd) noexcept;
    void cancel() noexcept;
    void release() noexcept;
};

}  // namespace ruvia::detail
