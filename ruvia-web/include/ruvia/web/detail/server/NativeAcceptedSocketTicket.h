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
class NativeAcceptedSocketTicket final {
public:
    using NativeHandle = asio::ip::tcp::socket::native_handle_type;
    static constexpr std::size_t kInvalidListener = static_cast<std::size_t>(-1);

    NativeAcceptedSocketTicket() noexcept = default;
    NativeAcceptedSocketTicket(asio::ip::tcp protocol, std::size_t listenerIndex,
        NativeHandle native) noexcept
        : protocol_(protocol),
          listenerIndex_(listenerIndex),
          native_(native),
          valid_(true) {}
    ~NativeAcceptedSocketTicket() {
        reset();
    }

    NativeAcceptedSocketTicket(const NativeAcceptedSocketTicket&) = delete;
    NativeAcceptedSocketTicket& operator=(const NativeAcceptedSocketTicket&) = delete;
    NativeAcceptedSocketTicket(NativeAcceptedSocketTicket&& other) noexcept {
        take(other);
    }
    NativeAcceptedSocketTicket& operator=(NativeAcceptedSocketTicket&& other) noexcept {
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
    [[nodiscard]] std::size_t listenerIndex() const noexcept {
        return listenerIndex_;
    }
    [[nodiscard]] NativeHandle nativeHandle() const noexcept {
        return native_;
    }

    [[nodiscard]] NativeHandle release() noexcept {
        valid_ = false;
        return std::exchange(native_, invalidNative());
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
        native_ = invalidNative();
    }

    [[nodiscard]] static constexpr NativeHandle invalidNative() noexcept {
#if defined(_WIN32)
        return INVALID_SOCKET;
#else
        return -1;
#endif
    }

private:
    void take(NativeAcceptedSocketTicket& other) noexcept {
        protocol_ = other.protocol_;
        listenerIndex_ = other.listenerIndex_;
        native_ = std::exchange(other.native_, invalidNative());
        valid_ = std::exchange(other.valid_, false);
    }

    asio::ip::tcp protocol_{asio::ip::tcp::v4()};
    std::size_t listenerIndex_{kInvalidListener};
    NativeHandle native_{invalidNative()};
    bool valid_{false};
};

}  // namespace ruvia::detail
