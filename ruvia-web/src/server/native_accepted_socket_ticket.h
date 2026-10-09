#pragma once

#include <cerrno>
#include <cstddef>
#include <utility>

#include <asio/ip/tcp.hpp>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#else
#include <unistd.h>
#endif

namespace ruvia::detail {

// Owns a detached native TCP socket until it is assigned to a worker-owned
// Asio socket. Destruction always closes an unconsumed descriptor exactly once.
class native_accepted_socket_ticket final {
public:
    using native_handle_type = asio::ip::tcp::socket::native_handle_type;
    static constexpr std::size_t invalid_listener = static_cast<std::size_t>(-1);

    native_accepted_socket_ticket() noexcept = default;
    native_accepted_socket_ticket(asio::ip::tcp protocol, std::size_t listener_index,
        native_handle_type native) noexcept
        : protocol_(protocol),
          listener_index_(listener_index),
          native_(native),
          valid_(true) {}
    ~native_accepted_socket_ticket() {
        reset();
    }

    native_accepted_socket_ticket(const native_accepted_socket_ticket&) = delete;
    native_accepted_socket_ticket& operator=(const native_accepted_socket_ticket&) = delete;
    native_accepted_socket_ticket(native_accepted_socket_ticket&& other) noexcept {
        take(other);
    }
    native_accepted_socket_ticket& operator=(native_accepted_socket_ticket&& other) noexcept {
        if (this != &other) {
            reset();
            take(other);
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept {
        return valid_;
    }
    [[nodiscard]] asio::ip::tcp protocol() const noexcept {
        return protocol_;
    }
    [[nodiscard]] std::size_t listener_index() const noexcept {
        return listener_index_;
    }
    [[nodiscard]] native_handle_type native_handle() const noexcept {
        return native_;
    }

    [[nodiscard]] native_handle_type release() noexcept {
        valid_ = false;
        return std::exchange(native_, invalid_native());
    }

    void reset() noexcept {
        if (!valid_) {
            return;
        }
        valid_ = false;
#if defined(_WIN32)
        (void)::closesocket(native_);
#else
        // Do not retry close after EINTR: the descriptor may already be reused.
        (void)::close(native_);
#endif
        native_ = invalid_native();
    }

    [[nodiscard]] static native_handle_type invalid_native() noexcept {
#if defined(_WIN32)
        return INVALID_SOCKET;
#else
        return -1;
#endif
    }

private:
    void take(native_accepted_socket_ticket& other) noexcept {
        protocol_ = other.protocol_;
        listener_index_ = other.listener_index_;
        native_ = std::exchange(other.native_, invalid_native());
        valid_ = std::exchange(other.valid_, false);
    }

    asio::ip::tcp protocol_{asio::ip::tcp::v4()};
    std::size_t listener_index_{invalid_listener};
    native_handle_type native_{invalid_native()};
    bool valid_{false};
};

}  // namespace ruvia::detail
