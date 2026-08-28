#pragma once

// Native file-open primitive for the web-layer server drivers. This performs real
// OS file I/O (::open / ::CreateFileW with an owning fd/HANDLE), so it belongs in
// ruvia-web, NOT in the pure sans-I/O ruvia-http protocol library. ruvia-http only
// owns the ResponseFileBody DESCRIPTOR (path + size/offset) used to frame
// Content-Length/Range; opening the file is a runtime driver concern.

#include "ruvia/http/detail/response/HttpResponseFileBody.h"

#include <system_error>
#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
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

struct ResponseFileSnapshot final {
    ResponseFileIdentity identity{ResponseFileIdentity::unchecked()};
    std::uint64_t size{0};
    std::uint64_t modifiedToken{0};
    std::time_t modifiedSeconds{0};
};

#if defined(__unix__) || defined(__APPLE__)
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

[[nodiscard]] inline ResponseFileSnapshot snapshotNativeFileHandle(int fd, std::error_code& ec) noexcept {
    struct stat status{};
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
    const std::array<std::uint64_t, 4> words{static_cast<std::uint64_t>(status.st_dev), static_cast<std::uint64_t>(status.st_ino), static_cast<std::uint64_t>(changedSeconds), static_cast<std::uint64_t>(changedNanoseconds)};
    ec = {};
    return ResponseFileSnapshot{ResponseFileIdentity::checked(words), static_cast<std::uint64_t>(status.st_size), static_cast<std::uint64_t>(modifiedSeconds) * UINT64_C(1000000000) + static_cast<std::uint64_t>(modifiedNanoseconds), static_cast<std::time_t>(modifiedSeconds)};
}

[[nodiscard]] inline ResponseFileSnapshot snapshotResponseFile(const HttpNativePathChar* path, std::error_code& ec) noexcept {
    NativeFileHandle input(::open(path, O_RDONLY | O_CLOEXEC));
    if (input.get() < 0) {
        ec = std::error_code(errno, std::system_category());
        return {};
    }
    return snapshotNativeFileHandle(input.get(), ec);
}

[[nodiscard]] inline NativeFileHandle openNativeFileForRead(ResponseFileBody file, std::error_code& ec, NativeFileOpenOptions = {}) noexcept {
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
        if (snapshot.identity != file.identity() || snapshot.size != file.size()) {
            ec = std::make_error_code(std::errc::state_not_recoverable);
            return NativeFileHandle();
        }
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

[[nodiscard]] inline std::uint64_t windowsFileTimeToken(LARGE_INTEGER value) noexcept {
    return static_cast<std::uint64_t>(value.QuadPart);
}

[[nodiscard]] inline std::time_t windowsFileTimeSeconds(LARGE_INTEGER value) noexcept {
    constexpr std::uint64_t kWindowsToUnixEpoch100ns = UINT64_C(116444736000000000);
    const auto ticks = windowsFileTimeToken(value);
    if (ticks <= kWindowsToUnixEpoch100ns) {
        return 0;
    }
    return static_cast<std::time_t>((ticks - kWindowsToUnixEpoch100ns) / UINT64_C(10000000));
}

[[nodiscard]] inline ResponseFileSnapshot snapshotNativeFileHandle(HANDLE handle, std::error_code& ec) noexcept {
    FILE_ID_INFO id{};
    FILE_STANDARD_INFO standard{};
    FILE_BASIC_INFO basic{};
    if (::GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) == 0 || ::GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)) == 0 || ::GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) == 0) {
        ec = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
        return {};
    }
    if (standard.Directory != FALSE || standard.EndOfFile.QuadPart < 0) {
        ec = std::make_error_code(std::errc::not_supported);
        return {};
    }
    std::uint64_t fileIdLow = 0;
    std::uint64_t fileIdHigh = 0;
    static_assert(sizeof(id.FileId.Identifier) == 16);
    std::memcpy(&fileIdLow, id.FileId.Identifier, sizeof(fileIdLow));
    std::memcpy(&fileIdHigh, id.FileId.Identifier + sizeof(fileIdLow), sizeof(fileIdHigh));
    const std::array<std::uint64_t, 4> words{static_cast<std::uint64_t>(id.VolumeSerialNumber), fileIdLow, fileIdHigh, windowsFileTimeToken(basic.ChangeTime)};
    ec = {};
    return ResponseFileSnapshot{ResponseFileIdentity::checked(words), static_cast<std::uint64_t>(standard.EndOfFile.QuadPart), windowsFileTimeToken(basic.LastWriteTime), windowsFileTimeSeconds(basic.LastWriteTime)};
}

[[nodiscard]] inline ResponseFileSnapshot snapshotResponseFile(const HttpNativePathChar* path, std::error_code& ec) noexcept {
    NativeFileHandle input(::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (input.get() == INVALID_HANDLE_VALUE) {
        ec = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
        return {};
    }
    return snapshotNativeFileHandle(input.get(), ec);
}

[[nodiscard]] inline NativeFileHandle openNativeFileForRead(ResponseFileBody file, std::error_code& ec, NativeFileOpenOptions options = {}) noexcept {
    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    if (options.overlapped) {
        flags |= FILE_FLAG_OVERLAPPED;
    }
    if (options.sequentialScan) {
        flags |= FILE_FLAG_SEQUENTIAL_SCAN;
    }

    NativeFileHandle input(::CreateFileW(file.nativePathCStr(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, flags, nullptr));
    if (input.get() == INVALID_HANDLE_VALUE) {
        ec = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
        return NativeFileHandle();
    }
    if (file.identity().requiresValidation()) {
        const auto snapshot = snapshotNativeFileHandle(input.get(), ec);
        if (ec) {
            return NativeFileHandle();
        }
        if (snapshot.identity != file.identity() || snapshot.size != file.size()) {
            ec = std::make_error_code(std::errc::state_not_recoverable);
            return NativeFileHandle();
        }
    }
    ec = {};
    return input;
}
#else
[[nodiscard]] inline ResponseFileSnapshot snapshotResponseFile(const HttpNativePathChar* path, std::error_code& ec) noexcept {
    static_cast<void>(path);
    ec = std::make_error_code(std::errc::not_supported);
    return {};
}
#endif

}  // namespace ruvia::detail
