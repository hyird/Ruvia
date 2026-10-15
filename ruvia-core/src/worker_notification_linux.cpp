#ifndef _WIN32

#ifndef __linux__
#error "worker_notification native backends are implemented for Linux and Windows only"
#endif

#include <fcntl.h>
#include <sys/eventfd.h>
#include <unistd.h>

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

}  // namespace

native_wake_pair::native_wake_pair(native_wake_pair&& other) noexcept
    : sender_(std::exchange(other.sender_, -1)),
      receiver_(std::exchange(other.receiver_, -1)) {}

native_wake_pair& native_wake_pair::operator=(native_wake_pair&& other) noexcept {
    std::swap(sender_, other.sender_);
    std::swap(receiver_, other.receiver_);
    return *this;
}

native_wake_pair::~native_wake_pair() {
    close_receiver();
    close_descriptor(sender_);
}

native_wake_signal native_wake_pair::signal() const noexcept {
    constexpr std::uint64_t token = 1;
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
            // eventfd returns EAGAIN only when its counter cannot be incremented
            // past UINT64_MAX-1. That nonzero counter is readable. A concurrent
            // read may consume it first, but the state's pending latch was
            // published before this write and is observed after draining.
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
    std::uint64_t counter = 0;
    for (std::size_t interrupted = 0; interrupted < 64;) {
        const auto bytes_read = ::read(receiver, &counter, sizeof(counter));
        if (bytes_read == static_cast<ssize_t>(sizeof(counter))) {
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
    pair.sender_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (pair.sender_ < 0) {
        throw_native_wake_error("eventfd", errno);
    }
    pair.receiver_ = fcntl(pair.sender_, F_DUPFD_CLOEXEC, 0);
    if (pair.receiver_ < 0) {
        throw_native_wake_error("fcntl(F_DUPFD_CLOEXEC)", errno);
    }
    return pair;
}

}  // namespace ruvia::detail

#endif
