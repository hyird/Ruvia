#pragma once

#include "HttpResponseFileBody.h"

#include <system_error>
#include <utility>

#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ruvia::detail {

struct NativeFileOpenOptions final {
    bool overlapped{false};
    bool sequentialScan{false};
};

#if defined(__linux__)
class NativeFileHandle final {
public:
    explicit NativeFileHandle(int fd = -1) noexcept
        : fd_(fd) {}

    ~NativeFileHandle() {
        reset();
    }

    NativeFileHandle(const NativeFileHandle&) = delete;
    NativeFileHandle& operator=(const NativeFileHandle&) = delete;

    NativeFileHandle(NativeFileHandle&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)) {}

    NativeFileHandle& operator=(NativeFileHandle&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept {
        return fd_;
    }

    [[nodiscard]] int release() noexcept {
        return std::exchange(fd_, -1);
    }

    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_;
};

[[nodiscard]] inline NativeFileHandle openNativeFileForRead(
    ResponseFileBody file,
    std::error_code& ec,
    NativeFileOpenOptions = {}) noexcept {
    NativeFileHandle input(::open(file.nativePathCStr(), O_RDONLY | O_CLOEXEC));
    if (input.get() < 0) {
        ec = std::error_code(errno, std::system_category());
        return NativeFileHandle();
    }
    ec = {};
    return input;
}
#elif defined(_WIN32)
class NativeFileHandle final {
public:
    explicit NativeFileHandle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept
        : handle_(handle) {}

    ~NativeFileHandle() {
        reset();
    }

    NativeFileHandle(const NativeFileHandle&) = delete;
    NativeFileHandle& operator=(const NativeFileHandle&) = delete;

    NativeFileHandle(NativeFileHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}

    NativeFileHandle& operator=(NativeFileHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

    [[nodiscard]] HANDLE release() noexcept {
        return std::exchange(handle_, INVALID_HANDLE_VALUE);
    }

    void reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept {
        if (handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_;
};

[[nodiscard]] inline NativeFileHandle openNativeFileForRead(
    ResponseFileBody file,
    std::error_code& ec,
    NativeFileOpenOptions options = {}) noexcept {
    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    if (options.overlapped) {
        flags |= FILE_FLAG_OVERLAPPED;
    }
    if (options.sequentialScan) {
        flags |= FILE_FLAG_SEQUENTIAL_SCAN;
    }

    NativeFileHandle input(::CreateFileW(
        file.nativePathCStr(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        flags,
        nullptr));
    if (input.get() == INVALID_HANDLE_VALUE) {
        ec = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
        return NativeFileHandle();
    }
    ec = {};
    return input;
}
#endif

}  // namespace ruvia::detail
