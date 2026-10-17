#ifndef _WIN32

#include <fcntl.h>
#ifdef __linux__
#include <sys/eventfd.h>
#endif
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <utility>

#include "worker_notification.h"

namespace ruvia::detail {

namespace {

void close_descriptor(int& descriptor) noexcept {
    const int owned = std::exchange(descriptor, -1);
    if (owned >= 0) {
        static_cast<void>(::close(owned));
    }
}

#ifndef __linux__
int configure_descriptor(int descriptor, int command, int value = 0) {
    for (std::size_t interrupted = 0; interrupted < 64; ++interrupted) {
        const int result = fcntl(descriptor, command, value);
        if (result >= 0) {
            return result;
        }
        if (errno != EINTR) {
            throw_native_wake_error("fcntl(worker notification pipe)", errno);
        }
    }
    throw_native_wake_error("fcntl(worker notification pipe)", EINTR);
}

void configure_pipe_descriptor(int descriptor) {
    const int descriptor_flags = configure_descriptor(descriptor, F_GETFD);
    static_cast<void>(configure_descriptor(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC));
    const int status_flags = configure_descriptor(descriptor, F_GETFL);
    static_cast<void>(configure_descriptor(descriptor, F_SETFL, status_flags | O_NONBLOCK));
}
#endif

}  // namespace

native_wake_pair::native_wake_pair(native_wake_pair&& other) noexcept
    : sender_(std::exchange(other.sender_, -1)),
      receiver_(std::exchange(other.receiver_, -1))
#ifndef __linux__
      ,
      receiver_guard_(std::exchange(other.receiver_guard_, -1))
#endif
{
}

native_wake_pair& native_wake_pair::operator=(native_wake_pair&& other) noexcept {
    std::swap(sender_, other.sender_);
    std::swap(receiver_, other.receiver_);
#ifndef __linux__
    std::swap(receiver_guard_, other.receiver_guard_);
#endif
    return *this;
}

native_wake_pair::~native_wake_pair() {
    close_receiver();
    close_descriptor(sender_);
#ifndef __linux__
    close_descriptor(receiver_guard_);
#endif
}

native_wake_signal native_wake_pair::signal() const noexcept {
#ifdef __linux__
    constexpr std::uint64_t token = 1;
#else
    constexpr char token = '\x01';
#endif
    for (std::size_t interrupted = 0; interrupted < 64;) {
        const auto written = ::write(sender_, &token, sizeof(token));
        if (written == static_cast<ssize_t>(sizeof(token))) {
            return native_wake_signal::signaled;
        }
        if (written < 0 && errno == EINTR) {
            ++interrupted;
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // A full eventfd or self-pipe is already readable. The pending
            // latch was published before this write and is observed after
            // consuming native tokens, including when a read races this write.
            return native_wake_signal::full;
        }
        return native_wake_signal::failed;
    }
    return native_wake_signal::failed;
}

void native_wake_pair::close_receiver() noexcept {
    close_descriptor(receiver_);
}

bool drain_native_receiver(native_wake_handle receiver, std::error_code& error) noexcept {
#ifdef __linux__
    std::uint64_t counter = 0;
#else
    std::array<char, 4096> tokens;
#endif
    for (std::size_t interrupted = 0; interrupted < 64;) {
#ifdef __linux__
        const auto bytes_read = ::read(receiver, &counter, sizeof(counter));
        if (bytes_read == static_cast<ssize_t>(sizeof(counter))) {
#else
        const auto bytes_read = ::read(receiver, tokens.data(), tokens.size());
        if (bytes_read > 0) {
#endif
            return true;
        }
        if (bytes_read < 0 && errno == EINTR) {
            ++interrupted;
            continue;
        }
        if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return false;
        }
        if (bytes_read < 0) {
            error = std::error_code(errno, std::system_category());
        } else if (bytes_read == 0) {
            error = std::make_error_code(std::errc::connection_reset);
        } else {
            error = std::make_error_code(std::errc::io_error);
        }
        return false;
    }
    error = std::make_error_code(std::errc::interrupted);
    return false;
}

native_wake_pair make_native_wake_pair(std::chrono::milliseconds) {
    native_wake_pair pair;
#ifdef __linux__
    pair.sender_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (pair.sender_ < 0) {
        throw_native_wake_error("eventfd", errno);
    }
    pair.receiver_ = fcntl(pair.sender_, F_DUPFD_CLOEXEC, 0);
    if (pair.receiver_ < 0) {
        throw_native_wake_error("fcntl(F_DUPFD_CLOEXEC)", errno);
    }
#else
    int descriptors[2]{-1, -1};
    for (std::size_t interrupted = 0;; ++interrupted) {
        if (::pipe(descriptors) == 0) {
            break;
        }
        if (errno != EINTR || interrupted == 63) {
            throw_native_wake_error("pipe(worker notification)", errno);
        }
    }
    pair.receiver_ = descriptors[0];
    pair.sender_ = descriptors[1];
    configure_pipe_descriptor(pair.receiver_);
    configure_pipe_descriptor(pair.sender_);
    pair.receiver_guard_ = configure_descriptor(pair.receiver_, F_DUPFD);
    configure_pipe_descriptor(pair.receiver_guard_);
#endif
    return pair;
}

}  // namespace ruvia::detail

#endif
