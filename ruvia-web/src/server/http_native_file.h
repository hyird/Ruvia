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
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

#include "ruvia/http/http_native_path.h"
#include "ruvia/http/http_response_file.h"

#if defined(__unix__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#endif
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#error "ruvia-web file responses need POSIX file descriptors or Windows file handles"
#endif

namespace ruvia::detail {

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

#if defined(__unix__)
using native_file_type = int;

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

    [[nodiscard]] explicit operator bool() const noexcept {
        return fd_ >= 0;
    }

    [[nodiscard]] int get() const noexcept {
        return fd_;
    }

    [[nodiscard]] int release() noexcept {
        return std::exchange(fd_, -1);
    }

    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = -1;
    }

private:
    int fd_;
};

[[nodiscard]] inline std::error_code last_native_file_error() noexcept {
    return std::error_code(errno, std::system_category());
}

[[nodiscard]] inline response_file_snapshot snapshot_native_file_handle(
    int fd, std::error_code& ec) noexcept {
    struct stat status{};
    if (::fstat(fd, &status) != 0) {
        ec = last_native_file_error();
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

// The one open of a file by path; every response, index and read handle
// starts here.
[[nodiscard]] inline native_file_handle open_native_file(
    const http_native_path_char_type* path, std::error_code& ec) noexcept {
    native_file_handle input(::open(path, native_file_open_flags));
    ec = input ? std::error_code{} : last_native_file_error();
    return input;
}

inline void release_response_file_handle(http_response_file_handle::native_type value) noexcept {
    ::close(static_cast<int>(value));
}

[[nodiscard]] inline int borrowed_native_file(const http_response_file_handle& handle) noexcept {
    return static_cast<int>(handle.native());
}

[[nodiscard]] inline http_response_file_handle adopt_response_file_handle(native_file_handle input) noexcept {
    return http_response_file_handle(
        static_cast<http_response_file_handle::native_type>(input.release()), &release_response_file_handle);
}

// An independent handle to the same open file, so a read state that outlives
// the response (an abandoned pool read) never borrows the response's handle.
// Reads are positional, so the shared file offset is never relied on.
[[nodiscard]] inline native_file_handle duplicate_native_file(
    const http_response_file_handle& handle, std::error_code& ec) noexcept {
    native_file_handle duplicate(::fcntl(borrowed_native_file(handle), F_DUPFD_CLOEXEC, 0));
    ec = duplicate ? std::error_code{} : last_native_file_error();
    return duplicate;
}

#if defined(O_PATH)
inline constexpr int native_directory_open_flags = O_PATH | O_DIRECTORY | O_CLOEXEC;
#else
inline constexpr int native_directory_open_flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
#endif

// A directory handle that later opens resolve entries against, so a request
// walks only the path below it instead of the whole absolute path.
[[nodiscard]] inline native_file_handle open_native_directory(
    const http_native_path_char_type* path, std::error_code& ec) noexcept {
    native_file_handle directory(::open(path, native_directory_open_flags));
    ec = directory ? std::error_code{} : last_native_file_error();
    return directory;
}

// The filesystem instance of a directory: device and inode.
struct native_directory_identity final {
    std::uint64_t device_{0};
    std::uint64_t inode_{0};

    friend bool operator==(const native_directory_identity&, const native_directory_identity&) = default;
};

[[nodiscard]] inline native_directory_identity native_directory_identity_of(
    const struct stat& status, std::error_code& ec) noexcept {
    if (!S_ISDIR(status.st_mode)) {
        ec = std::make_error_code(std::errc::not_a_directory);
        return {};
    }
    ec = {};
    return {static_cast<std::uint64_t>(status.st_dev), static_cast<std::uint64_t>(status.st_ino)};
}

[[nodiscard]] inline native_directory_identity native_directory_handle_identity(
    int directory, std::error_code& ec) noexcept {
    struct stat status{};
    if (::fstat(directory, &status) != 0) {
        ec = last_native_file_error();
        return {};
    }
    return native_directory_identity_of(status, ec);
}

[[nodiscard]] inline native_directory_identity native_directory_path_identity(
    const http_native_path_char_type* path, std::error_code& ec) noexcept {
    struct stat status{};
    if (::stat(path, &status) != 0) {
        ec = last_native_file_error();
        return {};
    }
    return native_directory_identity_of(status, ec);
}

// Opens `relative` ('/'-separated, as an index records it) below `directory`
// one component at a time, refusing a symlink, "." or ".." in any component.
[[nodiscard]] inline native_file_handle open_native_file_beneath_by_components(
    int directory, std::string_view relative, std::error_code& ec) noexcept {
    // NAME_MAX of every supported filesystem; a longer component cannot exist.
    constexpr std::size_t max_component_bytes = 255;
    std::array<char, max_component_bytes + 1> component{};
    native_file_handle parent;
    for (;;) {
        const auto separator = relative.find('/');
        const auto name = relative.substr(0, separator);
        if (name.empty() || name == "." || name == "..") {
            ec = std::make_error_code(std::errc::permission_denied);
            return native_file_handle();
        }
        if (name.size() > max_component_bytes) {
            ec = std::make_error_code(std::errc::filename_too_long);
            return native_file_handle();
        }
        std::memcpy(component.data(), name.data(), name.size());
        component[name.size()] = '\0';
        const int at = parent ? parent.get() : directory;
        if (separator == std::string_view::npos) {
            native_file_handle input(::openat(at, component.data(), native_file_open_flags | O_NOFOLLOW));
            ec = input ? std::error_code{} : last_native_file_error();
            return input;
        }
        // O_DIRECTORY | O_NOFOLLOW fails on a symlink rather than opening it.
        native_file_handle next(::openat(at, component.data(), native_directory_open_flags | O_NOFOLLOW));
        if (!next) {
            ec = last_native_file_error();
            return native_file_handle();
        }
        parent = std::move(next);
        relative.remove_prefix(separator + 1);
    }
}

#if defined(__linux__) && defined(SYS_openat2)
// struct open_how and its resolve flags from the kernel ABI
// (include/uapi/linux/openat2.h), declared here so the build needs only the
// syscall number from the libc headers.
struct native_open_how final {
    std::uint64_t flags_;
    std::uint64_t mode_;
    std::uint64_t resolve_;
};
inline constexpr std::uint64_t native_resolve_no_magiclinks = 0x02;
inline constexpr std::uint64_t native_resolve_no_symlinks = 0x04;
inline constexpr std::uint64_t native_resolve_beneath = 0x08;

// Cleared once the kernel reports openat2 missing; the component walk then
// serves every later open in the process.
inline std::atomic<bool> native_openat2_available{true};
#endif

// Opens `relative` below `directory` without following a symlink in any
// component and without leaving `directory`: an index never records a
// symlink, so a path that now resolves through one was swapped after it was
// indexed and is refused. The handle is not validated here: the caller
// snapshots it (index) or checks it against the indexed identity (response).
[[nodiscard]] inline native_file_handle open_native_file_beneath(
    int directory, const http_native_path_char_type* relative, std::error_code& ec) noexcept {
#if defined(__linux__) && defined(SYS_openat2)
    if (native_openat2_available.load(std::memory_order_relaxed)) {
        const native_open_how how{.flags_ = static_cast<std::uint64_t>(native_file_open_flags | O_NOFOLLOW),
            .mode_ = 0,
            .resolve_ = native_resolve_beneath | native_resolve_no_symlinks | native_resolve_no_magiclinks};
        const auto opened = ::syscall(SYS_openat2, directory, relative, &how, sizeof(how));
        if (opened >= 0) {
            ec = {};
            return native_file_handle(static_cast<int>(opened));
        }
        // ENOSYS: kernel before 5.6; EPERM: a seccomp filter that refuses the
        // syscall. The component walk enforces the same rule.
        if (errno == ENOSYS) {
            native_openat2_available.store(false, std::memory_order_relaxed);
        } else if (errno != EPERM) {
            ec = last_native_file_error();
            return native_file_handle();
        }
    }
#endif
    return open_native_file_beneath_by_components(directory, relative, ec);
}

// Reads exactly `size` bytes at `offset` without moving the descriptor's file
// position, so a handle shared by several ranges needs no seek. A file that
// ends early is shorter than the snapshot it was framed from: it changed.
[[nodiscard]] inline std::error_code read_native_file_at(
    int fd, std::uint64_t offset, char* output, std::size_t size) noexcept {
    while (size != 0) {
        const auto count = ::pread(fd, output, size, static_cast<off_t>(offset));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return last_native_file_error();
        }
        if (count == 0) {
            return std::make_error_code(std::errc::state_not_recoverable);
        }
        output += count;
        offset += static_cast<std::uint64_t>(count);
        size -= static_cast<std::size_t>(count);
    }
    return {};
}

#if defined(__linux__)
// Reads only bytes already in the page cache (RWF_NOWAIT), never waiting for
// the disk; 0 when none are. A kernel without RWF_NOWAIT clears `supported`.
[[nodiscard]] inline std::size_t read_native_file_cached(
    int fd, std::uint64_t offset, char* output, std::size_t size, bool& supported) noexcept {
#if defined(SYS_preadv2) && defined(RWF_NOWAIT)
    if (!supported || offset > static_cast<std::uint64_t>((std::numeric_limits<off_t>::max)())) {
        return 0;
    }
    iovec buffer{output, size};
    // The kernel ABI splits the offset into unsigned longs; on 64-bit ABIs the
    // high word is zero.
    const auto low = static_cast<unsigned long>(offset);
    unsigned long high = 0;
    if constexpr (sizeof(unsigned long) < sizeof(offset)) {
        high = static_cast<unsigned long>(offset >> 32);
    }
    const auto count = ::syscall(SYS_preadv2, fd, &buffer, 1UL, low, high, RWF_NOWAIT);
    if (count < 0) {
        if (errno == ENOSYS || errno == EOPNOTSUPP || errno == EINVAL || errno == EPERM) {
            supported = false;
        }
        return 0;
    }
    return static_cast<std::size_t>(count);
#else
    static_cast<void>(fd);
    static_cast<void>(offset);
    static_cast<void>(output);
    static_cast<void>(size);
    supported = false;
    return 0;
#endif
}
#endif
#else
using native_file_type = HANDLE;

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

    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

    [[nodiscard]] HANDLE release() noexcept {
        return std::exchange(handle_, INVALID_HANDLE_VALUE);
    }

    void reset() noexcept {
        if (handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
        }
        handle_ = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_;
};

[[nodiscard]] inline std::error_code last_native_file_error() noexcept {
    return std::error_code(static_cast<int>(::GetLastError()), std::system_category());
}

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
        ec = last_native_file_error();
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

// FILE_SHARE_DELETE lets a served file be renamed or replaced while a handle
// to it stays open, as POSIX allows.
inline constexpr DWORD native_file_share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
inline constexpr DWORD native_file_open_flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN;

// The one open of a file by path; every response, index and read handle
// starts here.
[[nodiscard]] inline native_file_handle open_native_file(
    const http_native_path_char_type* path, std::error_code& ec) noexcept {
    native_file_handle input(::CreateFileW(
        path, GENERIC_READ, native_file_share_mode, nullptr, OPEN_EXISTING, native_file_open_flags, nullptr));
    ec = input ? std::error_code{} : last_native_file_error();
    return input;
}

inline void release_response_file_handle(http_response_file_handle::native_type value) noexcept {
    ::CloseHandle(reinterpret_cast<HANDLE>(value));
}

[[nodiscard]] inline HANDLE borrowed_native_file(const http_response_file_handle& handle) noexcept {
    return reinterpret_cast<HANDLE>(handle.native());
}

[[nodiscard]] inline http_response_file_handle adopt_response_file_handle(native_file_handle input) noexcept {
    return http_response_file_handle(
        reinterpret_cast<http_response_file_handle::native_type>(input.release()), &release_response_file_handle);
}

// An independent handle to the same open file, so a read state that outlives
// the response (an abandoned pool read) never borrows the response's handle.
// Reads carry their offset, so the shared file pointer is never relied on.
[[nodiscard]] inline native_file_handle duplicate_native_file(
    const http_response_file_handle& handle, std::error_code& ec) noexcept {
    HANDLE duplicate = nullptr;
    if (::DuplicateHandle(::GetCurrentProcess(), borrowed_native_file(handle), ::GetCurrentProcess(), &duplicate, 0,
            FALSE, DUPLICATE_SAME_ACCESS) == 0) {
        ec = last_native_file_error();
        return native_file_handle();
    }
    ec = {};
    return native_file_handle(duplicate);
}

// Reads exactly `size` bytes at `offset`. A synchronous handle honours the
// OVERLAPPED offset and completes inline, so ranges sharing a handle need no
// seek. A file that ends early is shorter than the snapshot it was framed
// from: it changed.
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
            return error == ERROR_HANDLE_EOF ? std::make_error_code(std::errc::state_not_recoverable)
                                             : std::error_code(static_cast<int>(error), std::system_category());
        }
        if (count == 0) {
            return std::make_error_code(std::errc::state_not_recoverable);
        }
        output += count;
        offset += count;
        size -= count;
    }
    return {};
}
#endif

// Opens `path` once and snapshots the handle it opened.
[[nodiscard]] inline opened_response_file open_response_file(
    const http_native_path_char_type* path, std::error_code& ec) noexcept {
    auto input = open_native_file(path, ec);
    if (ec) {
        return {};
    }
    const auto snapshot = snapshot_native_file_handle(input.get(), ec);
    if (ec) {
        return {};
    }
    return {adopt_response_file_handle(std::move(input)), snapshot};
}

// Whether `input` still holds the representation framed with `identity` and
// `size`. A file rewritten in place keeps its handle valid, so a handle opened
// at framing must be checked again before its bytes are sent under the framed
// validators; state_not_recoverable when it no longer matches.
[[nodiscard]] inline std::error_code validate_native_file(
    native_file_type input, http_response_file_identity identity, std::uint64_t size) noexcept {
    if (!identity.requires_validation()) {
        return {};
    }
    std::error_code error;
    const auto snapshot = snapshot_native_file_handle(input, error);
    if (error) {
        return error;
    }
    if (snapshot.identity_ != identity || snapshot.size_ != size) {
        return std::make_error_code(std::errc::state_not_recoverable);
    }
    return {};
}

[[nodiscard]] inline std::error_code validate_native_file(
    native_file_type input, http_response_file_view file) noexcept {
    return validate_native_file(input, file.identity(), file.size());
}

}  // namespace ruvia::detail
