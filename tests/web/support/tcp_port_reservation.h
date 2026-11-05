#pragma once

#include <cerrno>
#include <system_error>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#if defined(_WIN32)
#include <windows.h>

#include <cwchar>
#include <memory>
#endif

namespace ruvia::test {

// Select without reuse, then retain the port until every server using it has
// stopped. Never listen here: a listening reservation steals real test clients.
// Windows fixes sharing policy at bind time, so its transition to a reusable
// socket also holds a process-independent named lease across the rebind.
class tcp_port_reservation final {
public:
    explicit tcp_port_reservation(asio::io_context& io)
        : socket_(io) {
        socket_.open(asio::ip::tcp::v4());
#if defined(_WIN32)
        for (;;) {
            socket_.bind({asio::ip::address_v4::loopback(), 0});
            const auto selected = socket_.local_endpoint();
            wchar_t name[64];
            std::swprintf(name, 64, L"Global\\Ruvia.Test.TcpPort.%u", static_cast<unsigned>(selected.port()));
            ::SetLastError(ERROR_SUCCESS);
            port_lease candidate(::CreateEventW(nullptr, TRUE, FALSE, name));
            const auto error = ::GetLastError();
            if (!candidate) {
                throw std::system_error(static_cast<int>(error), asio::system_category(), "failed to reserve TCP test port lease");
            }
            if (error == ERROR_ALREADY_EXISTS) {
                // Another fixture is between exclusive selection and its
                // reusable bind. Do not join that fixture's listener group.
                socket_.close();
                socket_.open(asio::ip::tcp::v4());
                continue;
            }
            lease_ = std::move(candidate);
            socket_.close();
            socket_.open(asio::ip::tcp::v4());
            socket_.set_option(asio::socket_base::reuse_address(true));
            socket_.bind(selected);
            break;
        }
#else
        socket_.bind({asio::ip::address_v4::loopback(), 0});
        socket_.set_option(asio::socket_base::reuse_address(true));
#if defined(SO_REUSEPORT)
        const int enabled = 1;
        if (::setsockopt(socket_.native_handle(), SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) != 0) {
            throw std::system_error(errno, asio::system_category(), "failed to share reserved TCP test port");
        }
#else
        throw std::system_error(asio::error::operation_not_supported, "TCP test reservations require SO_REUSEPORT");
#endif
#endif
    }

    [[nodiscard]] asio::ip::tcp::endpoint endpoint() const {
        return socket_.local_endpoint();
    }

private:
#if defined(_WIN32)
    struct close_lease final {
        void operator()(void* handle) const noexcept {
            ::CloseHandle(handle);
        }
    };
    using port_lease = std::unique_ptr<void, close_lease>;
    // Destroy the bound socket first; the named object survives until the last
    // handle closes, including when a process exits without running destructors.
    port_lease lease_;
#endif
    asio::ip::tcp::acceptor socket_;
};

}  // namespace ruvia::test
