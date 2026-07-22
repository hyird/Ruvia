#pragma once

// Native file-open primitive for the web-layer server drivers. This performs real
// OS file I/O (::open with an owning file descriptor), so it belongs in
// ruvia-web, NOT in the pure sans-I/O ruvia-http protocol library. ruvia-http only
// owns the ResponseFileBody DESCRIPTOR (path + size/offset) used to frame
// Content-Length/Range; opening the file is a runtime driver concern.

#include "ruvia/http/detail/HttpResponseFileBody.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <fcntl.h>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace ruvia::detail {

struct ResponseFileSnapshot final {
    ResponseFileIdentity identity{ResponseFileIdentity::unchecked()};
    std::uint64_t size{0};
    std::uint64_t modifiedToken{0};
    std::time_t modifiedSeconds{0};
};

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

[[nodiscard]] inline ResponseFileSnapshot snapshotNativeFileHandle(
    int fd,
    std::error_code& ec) noexcept {
    struct stat status {};
    if (::fstat(fd, &status) != 0) {
        ec = std::error_code(errno, std::system_category());
        return {};
    }
    if (!S_ISREG(status.st_mode) || status.st_size < 0) {
        ec = std::make_error_code(std::errc::not_supported);
        return {};
    }
#if defined(__APPLE__)
    const auto modifiedSeconds = status.st_mtimespec.tv_sec;
    const auto modifiedNanoseconds = status.st_mtimespec.tv_nsec;
    const auto changedSeconds = status.st_ctimespec.tv_sec;
    const auto changedNanoseconds = status.st_ctimespec.tv_nsec;
#else
    const auto modifiedSeconds = status.st_mtim.tv_sec;
    const auto modifiedNanoseconds = status.st_mtim.tv_nsec;
    const auto changedSeconds = status.st_ctim.tv_sec;
    const auto changedNanoseconds = status.st_ctim.tv_nsec;
#endif
    const std::array<std::uint64_t, 4> words{
        static_cast<std::uint64_t>(status.st_dev),
        static_cast<std::uint64_t>(status.st_ino),
        static_cast<std::uint64_t>(changedSeconds),
        static_cast<std::uint64_t>(changedNanoseconds)};
    ec = {};
    return ResponseFileSnapshot{
        ResponseFileIdentity::checked(words),
        static_cast<std::uint64_t>(status.st_size),
        static_cast<std::uint64_t>(modifiedSeconds) * UINT64_C(1000000000) +
            static_cast<std::uint64_t>(modifiedNanoseconds),
        static_cast<std::time_t>(modifiedSeconds)};
}

[[nodiscard]] inline ResponseFileSnapshot snapshotResponseFile(
    const char* path,
    std::error_code& ec) noexcept {
    NativeFileHandle input(::open(path, O_RDONLY | O_CLOEXEC));
    if (input.get() < 0) {
        ec = std::error_code(errno, std::system_category());
        return {};
    }
    return snapshotNativeFileHandle(input.get(), ec);
}

[[nodiscard]] inline NativeFileHandle openNativeFileForRead(
    ResponseFileBody file,
    std::error_code& ec) noexcept {
    NativeFileHandle input(::open(file.nativePathCStr(), O_RDONLY | O_CLOEXEC));
    if (input.get() < 0) {
        ec = std::error_code(errno, std::system_category());
        return NativeFileHandle();
    }
    if (file.identity().requiresValidation()) {
        const auto snapshot = snapshotNativeFileHandle(input.get(), ec);
        if (ec) {
            return NativeFileHandle();
        }
        if (snapshot.identity != file.identity() ||
            snapshot.size != file.size()) {
            ec = std::make_error_code(std::errc::state_not_recoverable);
            return NativeFileHandle();
        }
    }
    ec = {};
    return input;
}

}  // namespace ruvia::detail
