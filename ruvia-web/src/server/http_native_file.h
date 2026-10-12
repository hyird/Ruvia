#pragma once

// Native file-open primitive for the web-layer server drivers. This performs real
// OS file I/O (::open / ::CreateFileW with an owning fd/HANDLE), so it belongs in
// ruvia-web, NOT in the pure sans-I/O ruvia-http protocol library. ruvia-http only
// owns the http_response_file_view DESCRIPTOR (path + size/offset) used to frame
// Content-Length/Range, plus an opaque http_response_file_handle token whose
// release function is supplied here; opening, reading and closing the file are
// runtime driver concerns.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <system_error>
#include <utility>

#include "ruvia/http/http_native_path.h"
#include "ruvia/http/http_response_file.h"

#if defined(__unix__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ruvia::detail {

struct native_file_open_options final {
    bool overlapped_{false};
    bool sequential_scan_{false};
};

struct response_file_snapshot final {
    http_response_file_identity identity_{http_response_file_identity::unchecked()};
    std::uint64_t size_{0};
    std::uint64_t modified_token_{0};
    std::time_t modified_seconds_{0};
};

// One open handle plus the metadata read from it. context::file frames the
// response from this snapshot and hands the same handle to the transport.
struct opened_response_file final {
    http_response_file_handle handle_;
    response_file_snapshot snapshot_;
};

inline constexpr std::uint64_t windows_to_unix_epoch100ns = UINT64_C(116444736000000000);
inline constexpr std::uint64_t windows_file_time_ticks_per_second = UINT64_C(10000000);

// FILETIME is 100ns ticks from 1601-01-01. Floor fractional seconds, including
// times before 1970; do not collapse them to the Unix epoch.
[[nodiscard]] inline std::time_t unix_seconds_from_windows_file_time_ticks(std::uint64_t ticks) noexcept {
    if (ticks < windows_to_unix_epoch100ns) {
        const auto before_epoch = windows_to_unix_epoch100ns - ticks;
        return -static_cast<std::time_t>(before_epoch / windows_file_time_ticks_per_second) -
               static_cast<std::time_t>(before_epoch % windows_file_time_ticks_per_second != 0);
    }
    return static_cast<std::time_t>(
        (ticks - windows_to_unix_epoch100ns) / windows_file_time_ticks_per_second);
}

#if defined(__unix__) || defined(_WIN32)
#if defined(__unix__)
using native_file_type = int;
#else
using native_file_type = HANDLE;
#endif

// Whether `input` still holds the representation `file` was framed from. A
// file rewritten in place keeps its handle valid, so a handle opened at
// framing must be checked again before its bytes are sent under the framed
// validators; state_not_recoverable when it no longer matches.
[[nodiscard]] inline std::error_code validate_native_file(
    native_file_type input, http_response_file_view file) noexcept;
#endif

#if defined(__unix__)
class native_file_handle final {
public:
    explicit native_file_handle(int fd = -1) noexcept
        : fd_(fd) {}

    ~native_file_handle() {
        reset();
    }

    native_file_handle(const native_file_handle&) = delete;
    native_file_handle& operator=(const native_file_handle&) = delete;

    native_file_handle(native_file_handle&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)) {}

    native_file_handle& operator=(native_file_handle&& other) noexcept {
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

[[nodiscard]] inline response_file_snapshot snapshot_native_file_handle(
    int fd, std::error_code& ec) noexcept {
    struct stat status{};
    if (::fstat(fd, &status) != 0) {
        ec = std::error_code(errno, std::system_category());
        return {};
    }
    if (!S_ISREG(status.st_mode) || status.st_size < 0) {
        ec = std::make_error_code(std::errc::not_supported);
        return {};
    }
    const auto modified_seconds = status.st_mtim.tv_sec;
    const auto modified_nanoseconds = status.st_mtim.tv_nsec;
    const auto changed_seconds = status.st_ctim.tv_sec;
    const auto changed_nanoseconds = status.st_ctim.tv_nsec;
    const std::array<std::uint64_t, 4> words{static_cast<std::uint64_t>(status.st_dev),
        static_cast<std::uint64_t>(status.st_ino), static_cast<std::uint64_t>(changed_seconds),
        static_cast<std::uint64_t>(changed_nanoseconds)};
    ec = {};
    return response_file_snapshot{http_response_file_identity::checked(words),
        static_cast<std::uint64_t>(status.st_size),
        static_cast<std::uint64_t>(modified_seconds) * UINT64_C(1000000000) +
            static_cast<std::uint64_t>(modified_nanoseconds),
        static_cast<std::time_t>(modified_seconds)};
}

// O_NONBLOCK keeps a FIFO (or another special file swapped in for a regular
// one) from blocking the worker in open() before the same-handle fstat rejects
// it; it has no effect on reads from a regular file.
inline constexpr int native_file_open_flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;

[[nodiscard]] inline response_file_snapshot snapshot_response_file(
    const http_native_path_char_type* path, std::error_code& ec) noexcept {
    native_file_handle input(::open(path, native_file_open_flags));
    if (input.get() < 0) {
        ec = std::error_code(errno, std::system_category());
        return {};
    }
    return snapshot_native_file_handle(input.get(), ec);
}

inline void release_response_file_handle(http_response_file_handle::native_type value) noexcept {
    ::close(static_cast<int>(value));
}

[[nodiscard]] inline int borrowed_native_file(const http_response_file_handle& handle) noexcept {
    return static_cast<int>(handle.native());
}

[[nodiscard]] inline opened_response_file open_response_file(
    const http_native_path_char_type* path, std::error_code& ec) noexcept {
    native_file_handle input(::open(path, native_file_open_flags));
    if (input.get() < 0) {
        ec = std::error_code(errno, std::system_category());
        return {};
    }
    const auto snapshot = snapshot_native_file_handle(input.get(), ec);
    if (ec) {
        return {};
    }
    return {http_response_file_handle(static_cast<http_response_file_handle::native_type>(input.release()),
                &release_response_file_handle),
        snapshot};
}

// Reads exactly `size` bytes at `offset` without moving the descriptor's file
// position, so a handle shared by several ranges needs no seek.
[[nodiscard]] inline std::error_code read_native_file_at(
    int fd, std::uint64_t offset, char* output, std::size_t size) noexcept {
    while (size != 0) {
        const auto count = ::pread(fd, output, size, static_cast<off_t>(offset));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::error_code(errno, std::system_category());
        }
        if (count == 0) {
            return std::make_error_code(std::errc::io_error);
        }
        output += count;
        offset += static_cast<std::uint64_t>(count);
        size -= static_cast<std::size_t>(count);
    }
    return {};
}

[[nodiscard]] inline native_file_handle open_native_file_for_read(
    http_response_file_view file, std::error_code& ec, native_file_open_options = {}) noexcept {
    native_file_handle input(::open(file.native_path_c_str(), native_file_open_flags));
    if (input.get() < 0) {
        ec = std::error_code(errno, std::system_category());
        return native_file_handle();
    }
    ec = validate_native_file(input.get(), file);
    if (ec) {
        return native_file_handle();
    }
    return input;
}
#elif defined(_WIN32)
class native_file_handle final {
public:
    explicit native_file_handle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept
        : handle_(handle) {}

    ~native_file_handle() {
        reset();
    }

    native_file_handle(const native_file_handle&) = delete;
    native_file_handle& operator=(const native_file_handle&) = delete;

    native_file_handle(native_file_handle&& other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}

    native_file_handle& operator=(native_file_handle&& other) noexcept {
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

[[nodiscard]] inline std::uint64_t windows_file_time_token(LARGE_INTEGER value) noexcept {
    return static_cast<std::uint64_t>(value.QuadPart);
}

[[nodiscard]] inline std::time_t windows_file_time_seconds(LARGE_INTEGER value) noexcept {
    return unix_seconds_from_windows_file_time_ticks(windows_file_time_token(value));
}

[[nodiscard]] inline response_file_snapshot snapshot_native_file_handle(
    HANDLE handle, std::error_code& ec) noexcept {
    FILE_ID_INFO id{};
    FILE_STANDARD_INFO standard{};
    FILE_BASIC_INFO basic{};
    if (::GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) == 0 ||
        ::GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)) ==
            0 ||
        ::GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) == 0) {
        ec = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
        return {};
    }
    if (standard.Directory != FALSE || standard.EndOfFile.QuadPart < 0) {
        ec = std::make_error_code(std::errc::not_supported);
        return {};
    }
    std::uint64_t file_id_low = 0;
    std::uint64_t file_id_high = 0;
    static_assert(sizeof(id.FileId.Identifier) == 16);
    std::memcpy(&file_id_low, id.FileId.Identifier, sizeof(file_id_low));
    std::memcpy(&file_id_high, id.FileId.Identifier + sizeof(file_id_low), sizeof(file_id_high));
    const std::array<std::uint64_t, 4> words{static_cast<std::uint64_t>(id.VolumeSerialNumber),
        file_id_low, file_id_high, windows_file_time_token(basic.ChangeTime)};
    ec = {};
    return response_file_snapshot{http_response_file_identity::checked(words),
        static_cast<std::uint64_t>(standard.EndOfFile.QuadPart),
        windows_file_time_token(basic.LastWriteTime), windows_file_time_seconds(basic.LastWriteTime)};
}

[[nodiscard]] inline response_file_snapshot snapshot_response_file(
    const http_native_path_char_type* path, std::error_code& ec) noexcept {
    native_file_handle input(
        ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (input.get() == INVALID_HANDLE_VALUE) {
        ec = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
        return {};
    }
    return snapshot_native_file_handle(input.get(), ec);
}

inline void release_response_file_handle(http_response_file_handle::native_type value) noexcept {
    ::CloseHandle(reinterpret_cast<HANDLE>(value));
}

[[nodiscard]] inline HANDLE borrowed_native_file(const http_response_file_handle& handle) noexcept {
    return reinterpret_cast<HANDLE>(handle.native());
}

[[nodiscard]] inline opened_response_file open_response_file(
    const http_native_path_char_type* path, std::error_code& ec) noexcept {
    native_file_handle input(
        ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (input.get() == INVALID_HANDLE_VALUE) {
        ec = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
        return {};
    }
    const auto snapshot = snapshot_native_file_handle(input.get(), ec);
    if (ec) {
        return {};
    }
    return {http_response_file_handle(reinterpret_cast<http_response_file_handle::native_type>(input.release()),
                &release_response_file_handle),
        snapshot};
}

// Reads exactly `size` bytes at `offset`. A synchronous handle honours the
// OVERLAPPED offset and completes inline, so ranges sharing a handle need no
// seek.
[[nodiscard]] inline std::error_code read_native_file_at(
    HANDLE handle, std::uint64_t offset, char* output, std::size_t size) noexcept {
    while (size != 0) {
        OVERLAPPED position{};
        position.Offset = static_cast<DWORD>(offset & UINT64_C(0xffffffff));
        position.OffsetHigh = static_cast<DWORD>(offset >> 32);
        const auto requested = static_cast<DWORD>(
            std::min<std::size_t>(size, static_cast<std::size_t>(UINT32_C(0x7fffffff))));
        DWORD count = 0;
        if (::ReadFile(handle, output, requested, &count, &position) == 0) {
            const auto error = ::GetLastError();
            return error == ERROR_HANDLE_EOF ? std::make_error_code(std::errc::io_error)
                                             : std::error_code(static_cast<int>(error), std::system_category());
        }
        if (count == 0) {
            return std::make_error_code(std::errc::io_error);
        }
        output += count;
        offset += count;
        size -= count;
    }
    return {};
}

[[nodiscard]] inline native_file_handle open_native_file_for_read(
    http_response_file_view file, std::error_code& ec, native_file_open_options options = {}) noexcept {
    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    if (options.overlapped_) {
        flags |= FILE_FLAG_OVERLAPPED;
    }
    if (options.sequential_scan_) {
        flags |= FILE_FLAG_SEQUENTIAL_SCAN;
    }

    native_file_handle input(::CreateFileW(file.native_path_c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, flags,
        nullptr));
    if (input.get() == INVALID_HANDLE_VALUE) {
        ec = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
        return native_file_handle();
    }
    ec = validate_native_file(input.get(), file);
    if (ec) {
        return native_file_handle();
    }
    return input;
}
#else
[[nodiscard]] inline response_file_snapshot snapshot_response_file(
    const http_native_path_char_type* path, std::error_code& ec) noexcept {
    static_cast<void>(path);
    ec = std::make_error_code(std::errc::not_supported);
    return {};
}

[[nodiscard]] inline opened_response_file open_response_file(
    const http_native_path_char_type* path, std::error_code& ec) noexcept {
    static_cast<void>(path);
    ec = std::make_error_code(std::errc::not_supported);
    return {};
}
#endif

#if defined(__unix__) || defined(_WIN32)
[[nodiscard]] inline std::error_code validate_native_file(
    native_file_type input, http_response_file_view file) noexcept {
    if (!file.identity().requires_validation()) {
        return {};
    }
    std::error_code error;
    const auto snapshot = snapshot_native_file_handle(input, error);
    if (error) {
        return error;
    }
    if (snapshot.identity_ != file.identity() || snapshot.size_ != file.size()) {
        return std::make_error_code(std::errc::state_not_recoverable);
    }
    return {};
}
#endif

}  // namespace ruvia::detail
