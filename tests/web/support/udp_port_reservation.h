#pragma once

#include <cerrno>
#include <system_error>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>

#if defined(_WIN32)
#include <windows.h>

#include <cwchar>
#include <memory>
#endif

namespace ruvia::test {

// Selects a free loopback UDP port and keeps other test fixtures off it until
// every server using it has stopped.
//
// POSIX: selection binds without reuse, then the socket joins the port's
// SO_REUSEPORT group so servers can bind the same port. A plain group member
// would receive its hash share of client datagrams, so the reservation is
// connected to a fixed peer that never sends (127.0.0.1:9, discard): Linux
// scores a connected UDP socket only for datagrams from that exact peer and
// otherwise delivers to the unconnected server sockets, keeping each client
// 4-tuple on one server. Verified on Linux 6.12 with four reuseport servers
// and 2000 datagrams from 500 client ports: the connected reservation
// received none and every datagram reached a server, while an unconnected
// reservation took about a fifth of them.
//
// Windows: SO_REUSEPORT does not exist and a server binds one UDP socket; a
// second socket bound to the port would split its datagrams. The reservation
// therefore releases the UDP port after selection and keeps only a
// process-independent named lease, which stops other fixtures from selecting
// the port. A process outside the test fixtures may still take it before the
// server binds; that bind then fails loudly instead of sharing traffic.
class udp_port_reservation final {
public:
    explicit udp_port_reservation(asio::io_context& io)
        : socket_(io) {
        socket_.open(asio::ip::udp::v4());
#if defined(_WIN32)
        for (;;) {
            socket_.bind({asio::ip::address_v4::loopback(), 0});
            const auto selected = socket_.local_endpoint();
            wchar_t name[64];
            std::swprintf(name, 64, L"Global\\Ruvia.Test.UdpPort.%u", static_cast<unsigned>(selected.port()));
            ::SetLastError(ERROR_SUCCESS);
            port_lease candidate(::CreateEventW(nullptr, TRUE, FALSE, name));
            const auto error = ::GetLastError();
            if (!candidate) {
                throw std::system_error(static_cast<int>(error), asio::system_category(), "failed to reserve UDP test port lease");
            }
            socket_.close();
            if (error == ERROR_ALREADY_EXISTS) {
                // Another fixture holds this port number between its
                // selection and its server's bind.
                socket_.open(asio::ip::udp::v4());
                continue;
            }
            lease_ = std::move(candidate);
            endpoint_ = selected;
            break;
        }
#else
        socket_.bind({asio::ip::address_v4::loopback(), 0});
        endpoint_ = socket_.local_endpoint();
        socket_.set_option(asio::socket_base::reuse_address(true));
#if defined(SO_REUSEPORT)
        const int enabled = 1;
        if (::setsockopt(socket_.native_handle(), SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) != 0) {
            throw std::system_error(errno, asio::system_category(), "failed to share reserved UDP test port");
        }
#else
        throw std::system_error(asio::error::operation_not_supported, "UDP test reservations require SO_REUSEPORT");
#endif
        socket_.connect({asio::ip::address_v4::loopback(), 9});
#endif
    }

    [[nodiscard]] asio::ip::udp::endpoint endpoint() const {
        return endpoint_;
    }

private:
#if defined(_WIN32)
    struct close_lease final {
        void operator()(void* handle) const noexcept {
            ::CloseHandle(handle);
        }
    };
    using port_lease = std::unique_ptr<void, close_lease>;
    // The named object survives until the last handle closes, including when
    // a process exits without running destructors.
    port_lease lease_;
#endif
    asio::ip::udp::socket socket_;
    asio::ip::udp::endpoint endpoint_;
};

}  // namespace ruvia::test
