#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "blocking_pool.h"
#include "borrowed_text.h"
#include "http_response.h"
#include "stop_token.h"
#include "task.h"
#include "worker_ref.h"

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
#include <winsock2.h>
// windows.h would otherwise pull in winsock.h, which conflicts with Asio's winsock2.h.
#include <windows.h>
#else
#error "ruvia-web file responses need POSIX file descriptors or Windows file handles"
#endif

// Native file I/O behind file response bodies: owning descriptors, opens that
// stay beneath a static root, identity snapshots, positional reads, and the
// blocking-pool reader every transport streams file bodies from.
namespace ruvia::detail {

// Native file-open primitive for the web-layer server drivers. This performs real
// OS file I/O (::open / ::CreateFileW with an owning fd/HANDLE), so it belongs in
// ruvia-web, NOT in the pure sans-I/O ruvia-http protocol library. ruvia-http only
// owns the http_response_file_view DESCRIPTOR (path + size/offset) used to frame
// Content-Length/Range, plus an opaque http_response_file_descriptor token whose
// release function is supplied here; opening, reading and closing the file are
// runtime driver concerns.

struct response_file_snapshot final {
    http_response_file_identity identity_{http_response_file_identity::unchecked()};
    std::uint64_t size_{0};
    std::uint64_t modified_token_{0};
    std::time_t modified_seconds_{0};
};

// One open handle plus the metadata read from it. context::file frames the
// response from this snapshot and hands the same handle to the transport.
struct opened_response_file final {
    http_response_file_descriptor handle_;
    response_file_snapshot snapshot_;
};

inline constexpr std::uint64_t windows_to_unix_epoch100ns = UINT64_C(116444736000000000);
inline constexpr std::uint64_t windows_file_time_ticks_per_second = UINT64_C(10000000);

// FILETIME is 100ns ticks from 1601-01-01. Floor fractional seconds, including
// times before 1970; do not collapse them to the Unix epoch.
[[nodiscard]] inline std::time_t unix_seconds_from_windows_file_time_ticks(std::uint64_t ticks) noexcept {
    if (ticks < windows_to_unix_epoch100ns) {
        const auto before_epoch = windows_to_unix_epoch100ns - ticks;
        return -static_cast<std::time_t>(before_epoch / windows_file_time_ticks_per_second) - static_cast<std::time_t>(before_epoch % windows_file_time_ticks_per_second != 0);
    }
    return static_cast<std::time_t>((ticks - windows_to_unix_epoch100ns) / windows_file_time_ticks_per_second);
}

#if defined(__unix__)
using native_file_type = int;

class native_file_descriptor final {
public:
    explicit native_file_descriptor(int fd = -1) noexcept :
        fd_(fd) {}

    ~native_file_descriptor() {
        reset();
    }

    native_file_descriptor(const native_file_descriptor&) = delete;
    native_file_descriptor& operator=(const native_file_descriptor&) = delete;

    native_file_descriptor(native_file_descriptor&& other) noexcept :
        fd_(std::exchange(other.fd_, -1)) {}

    native_file_descriptor& operator=(native_file_descriptor&& other) noexcept {
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

[[nodiscard]] inline response_file_snapshot snapshot_native_file_handle(int fd, std::error_code& ec) noexcept {
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
    const std::array<std::uint64_t, 4> words{static_cast<std::uint64_t>(status.st_dev), static_cast<std::uint64_t>(status.st_ino), static_cast<std::uint64_t>(changed_seconds), static_cast<std::uint64_t>(changed_nanoseconds)};
    ec = {};
    return response_file_snapshot{http_response_file_identity::checked(words), static_cast<std::uint64_t>(status.st_size), static_cast<std::uint64_t>(modified_seconds) * UINT64_C(1000000000) + static_cast<std::uint64_t>(modified_nanoseconds), static_cast<std::time_t>(modified_seconds)};
}

// O_NONBLOCK keeps a FIFO (or another special file swapped in for a regular
// one) from blocking the worker in open() before the same-handle fstat rejects
// it; it has no effect on reads from a regular file.
inline constexpr int native_file_open_flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;

// The one open of a file by path; every response, index and read handle
// starts here.
[[nodiscard]] inline native_file_descriptor open_native_file(const http_native_path_char_type* path, std::error_code& ec) noexcept {
    native_file_descriptor input(::open(path, native_file_open_flags));
    ec = input ? std::error_code{} : last_native_file_error();
    return input;
}

inline void release_response_file_handle(http_response_file_descriptor::native_type value) noexcept {
    ::close(static_cast<int>(value));
}

[[nodiscard]] inline int borrowed_native_file(const http_response_file_descriptor& handle) noexcept {
    return static_cast<int>(handle.native());
}

[[nodiscard]] inline http_response_file_descriptor adopt_response_file_handle(native_file_descriptor input) noexcept {
    return http_response_file_descriptor(static_cast<http_response_file_descriptor::native_type>(input.release()), &release_response_file_handle);
}

// An independent handle to the same open file, so a read state that outlives
// the response (an abandoned pool read) never borrows the response's handle.
// Reads are positional, so the shared file offset is never relied on.
[[nodiscard]] inline native_file_descriptor duplicate_native_file(const http_response_file_descriptor& handle, std::error_code& ec) noexcept {
    native_file_descriptor duplicate(::fcntl(borrowed_native_file(handle), F_DUPFD_CLOEXEC, 0));
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
[[nodiscard]] inline native_file_descriptor open_native_directory(const http_native_path_char_type* path, std::error_code& ec) noexcept {
    native_file_descriptor directory(::open(path, native_directory_open_flags));
    ec = directory ? std::error_code{} : last_native_file_error();
    return directory;
}

// The filesystem instance of a directory: device and inode.
struct native_directory_identity final {
    std::uint64_t device_{0};
    std::uint64_t inode_{0};

    friend bool operator==(const native_directory_identity&, const native_directory_identity&) = default;
};

[[nodiscard]] inline native_directory_identity native_directory_identity_of(const struct stat& status, std::error_code& ec) noexcept {
    if (!S_ISDIR(status.st_mode)) {
        ec = std::make_error_code(std::errc::not_a_directory);
        return {};
    }
    ec = {};
    return {static_cast<std::uint64_t>(status.st_dev), static_cast<std::uint64_t>(status.st_ino)};
}

[[nodiscard]] inline native_directory_identity native_directory_handle_identity(int directory, std::error_code& ec) noexcept {
    struct stat status{};
    if (::fstat(directory, &status) != 0) {
        ec = last_native_file_error();
        return {};
    }
    return native_directory_identity_of(status, ec);
}

[[nodiscard]] inline native_directory_identity native_directory_path_identity(const http_native_path_char_type* path, std::error_code& ec) noexcept {
    struct stat status{};
    if (::stat(path, &status) != 0) {
        ec = last_native_file_error();
        return {};
    }
    return native_directory_identity_of(status, ec);
}

// Opens `relative` ('/'-separated, as an index records it) below `directory`
// one component at a time, refusing a symlink, "." or ".." in any component.
[[nodiscard]] inline native_file_descriptor open_native_file_beneath_by_components(int directory, std::string_view relative, std::error_code& ec) noexcept {
    // NAME_MAX of every supported filesystem; a longer component cannot exist.
    constexpr std::size_t max_component_bytes = 255;
    std::array<char, max_component_bytes + 1> component{};
    native_file_descriptor parent;
    for (;;) {
        const auto separator = relative.find('/');
        const auto name = relative.substr(0, separator);
        if (name.empty() || name == "." || name == "..") {
            ec = std::make_error_code(std::errc::permission_denied);
            return native_file_descriptor();
        }
        if (name.size() > max_component_bytes) {
            ec = std::make_error_code(std::errc::filename_too_long);
            return native_file_descriptor();
        }
        std::memcpy(component.data(), name.data(), name.size());
        component[name.size()] = '\0';
        const int at = parent ? parent.get() : directory;
        if (separator == std::string_view::npos) {
            native_file_descriptor input(::openat(at, component.data(), native_file_open_flags | O_NOFOLLOW));
            ec = input ? std::error_code{} : last_native_file_error();
            return input;
        }
        // O_DIRECTORY | O_NOFOLLOW fails on a symlink rather than opening it.
        native_file_descriptor next(::openat(at, component.data(), native_directory_open_flags | O_NOFOLLOW));
        if (!next) {
            ec = last_native_file_error();
            return native_file_descriptor();
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
[[nodiscard]] inline native_file_descriptor open_native_file_beneath(int directory, const http_native_path_char_type* relative, std::error_code& ec) noexcept {
#if defined(__linux__) && defined(SYS_openat2)
    if (native_openat2_available.load(std::memory_order_relaxed)) {
        const native_open_how how{.flags_ = static_cast<std::uint64_t>(native_file_open_flags | O_NOFOLLOW), .mode_ = 0, .resolve_ = native_resolve_beneath | native_resolve_no_symlinks | native_resolve_no_magiclinks};
        const auto opened = ::syscall(SYS_openat2, directory, relative, &how, sizeof(how));
        if (opened >= 0) {
            ec = {};
            return native_file_descriptor(static_cast<int>(opened));
        }
        // ENOSYS: kernel before 5.6; EPERM: a seccomp filter that refuses the
        // syscall. The component walk enforces the same rule.
        if (errno == ENOSYS) {
            native_openat2_available.store(false, std::memory_order_relaxed);
        } else if (errno != EPERM) {
            ec = last_native_file_error();
            return native_file_descriptor();
        }
    }
#endif
    return open_native_file_beneath_by_components(directory, relative, ec);
}

// Reads exactly `size` bytes at `offset` without moving the descriptor's file
// position, so a handle shared by several ranges needs no seek. A file that
// ends early is shorter than the snapshot it was framed from: it changed.
[[nodiscard]] inline std::error_code read_native_file_at(int fd, std::uint64_t offset, char* output, std::size_t size) noexcept {
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
[[nodiscard]] inline std::size_t read_native_file_cached(int fd, std::uint64_t offset, char* output, std::size_t size, bool& supported) noexcept {
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

class native_file_descriptor final {
public:
    explicit native_file_descriptor(HANDLE handle = INVALID_HANDLE_VALUE) noexcept :
        handle_(handle) {}

    ~native_file_descriptor() {
        reset();
    }

    native_file_descriptor(const native_file_descriptor&) = delete;
    native_file_descriptor& operator=(const native_file_descriptor&) = delete;

    native_file_descriptor(native_file_descriptor&& other) noexcept :
        handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}

    native_file_descriptor& operator=(native_file_descriptor&& other) noexcept {
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

[[nodiscard]] inline response_file_snapshot snapshot_native_file_handle(HANDLE handle, std::error_code& ec) noexcept {
    FILE_ID_INFO id{};
    FILE_STANDARD_INFO standard{};
    FILE_BASIC_INFO basic{};
    if (::GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) == 0 || ::GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)) == 0 || ::GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) == 0) {
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
    const std::array<std::uint64_t, 4> words{static_cast<std::uint64_t>(id.VolumeSerialNumber), file_id_low, file_id_high, windows_file_time_token(basic.ChangeTime)};
    ec = {};
    return response_file_snapshot{http_response_file_identity::checked(words), static_cast<std::uint64_t>(standard.EndOfFile.QuadPart), windows_file_time_token(basic.LastWriteTime), windows_file_time_seconds(basic.LastWriteTime)};
}

// FILE_SHARE_DELETE lets a served file be renamed or replaced while a handle
// to it stays open, as POSIX allows.
inline constexpr DWORD native_file_share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
inline constexpr DWORD native_file_open_flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN;

// The one open of a file by path; every response, index and read handle
// starts here.
[[nodiscard]] inline native_file_descriptor open_native_file(const http_native_path_char_type* path, std::error_code& ec) noexcept {
    native_file_descriptor input(::CreateFileW(path, GENERIC_READ, native_file_share_mode, nullptr, OPEN_EXISTING, native_file_open_flags, nullptr));
    ec = input ? std::error_code{} : last_native_file_error();
    return input;
}

inline void release_response_file_handle(http_response_file_descriptor::native_type value) noexcept {
    ::CloseHandle(reinterpret_cast<HANDLE>(value));
}

[[nodiscard]] inline HANDLE borrowed_native_file(const http_response_file_descriptor& handle) noexcept {
    return reinterpret_cast<HANDLE>(handle.native());
}

[[nodiscard]] inline http_response_file_descriptor adopt_response_file_handle(native_file_descriptor input) noexcept {
    return http_response_file_descriptor(reinterpret_cast<http_response_file_descriptor::native_type>(input.release()), &release_response_file_handle);
}

// An independent handle to the same open file, so a read state that outlives
// the response (an abandoned pool read) never borrows the response's handle.
// Reads carry their offset, so the shared file pointer is never relied on.
[[nodiscard]] inline native_file_descriptor duplicate_native_file(const http_response_file_descriptor& handle, std::error_code& ec) noexcept {
    HANDLE duplicate = nullptr;
    if (::DuplicateHandle(::GetCurrentProcess(), borrowed_native_file(handle), ::GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS) == 0) {
        ec = last_native_file_error();
        return native_file_descriptor();
    }
    ec = {};
    return native_file_descriptor(duplicate);
}

// Reads exactly `size` bytes at `offset`. A synchronous handle honours the
// OVERLAPPED offset and completes inline, so ranges sharing a handle need no
// seek. A file that ends early is shorter than the snapshot it was framed
// from: it changed.
[[nodiscard]] inline std::error_code read_native_file_at(HANDLE handle, std::uint64_t offset, char* output, std::size_t size) noexcept {
    while (size != 0) {
        OVERLAPPED position{};
        position.Offset = static_cast<DWORD>(offset & UINT64_C(0xffffffff));
        position.OffsetHigh = static_cast<DWORD>(offset >> 32);
        const auto requested = static_cast<DWORD>(std::min<std::size_t>(size, static_cast<std::size_t>(UINT32_C(0x7fffffff))));
        DWORD count = 0;
        if (::ReadFile(handle, output, requested, &count, &position) == 0) {
            const auto error = ::GetLastError();
            return error == ERROR_HANDLE_EOF ? std::make_error_code(std::errc::state_not_recoverable) : std::error_code(static_cast<int>(error), std::system_category());
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
[[nodiscard]] inline opened_response_file open_response_file(const http_native_path_char_type* path, std::error_code& ec) noexcept {
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
[[nodiscard]] inline std::error_code validate_native_file(native_file_type input, http_response_file_identity identity, std::uint64_t size) noexcept {
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

[[nodiscard]] inline std::error_code validate_native_file(native_file_type input, http_response_file_view file) noexcept {
    return validate_native_file(input, file.identity(), file.size());
}

// The one reader for file response bodies (a whole file, a byte range, or
// multipart/byteranges parts interleaved with in-memory boundaries) on every
// transport. Only a single large range on Windows plain TCP leaves through a
// kernel TransmitFile instead (http1_file_response_body).
//
// Opening and blocking reads run on the worker's blocking pool, never on the
// worker. Each round trip reads ahead up to response_file_read_ahead_bytes;
// Linux tries later reads with RWF_NOWAIT first. The owned read state moves
// through the pool, without copying its buffered bytes. A worker without a
// pool (application::blocking_pool(nullptr)) cannot produce a file body: the
// read fails with blocking_status::unavailable on every platform.
//
// A response framed from an open handle (context::file, static_root) is read
// through a duplicate of that handle, so the path is never resolved again.
//
// Every file range is re-validated against its snapshot identity on the read
// that completes it, so the final bytes of a range are never released for a
// file that changed while being sent. The first file range is validated
// before anything is yielded, so a transport can answer a failure with an
// error response while its head is still unsent.

// Bytes read per blocking round trip. Kept within the process resource's
// pooled block range, so the buffer is recycled across responses.
inline constexpr std::size_t response_file_read_ahead_bytes = 64 * 1024;

// Where a file body's blocking reads run -- the worker's blocking pool -- and
// what bounds or abandons them.
struct response_file_io final {
    worker_ref worker_;
    stop_token stop_;
    // Bounds each read the response waits on. Servers pass write_timeout_: the
    // next write waits for the read. An abandoned read keeps running on the
    // pool and frees its own handle and buffer; the response does not wait.
    std::optional<timeout_duration> timeout_;
};

// Why a file body could not be produced.
class response_file_failure final {
public:
    // The read ran: the open, read or snapshot validation failed with `error`.
    [[nodiscard]] static response_file_failure read_failed(std::error_code error);
    // The read never completed: the pool refused, timed out or abandoned it.
    [[nodiscard]] static response_file_failure read_not_completed(blocking_status status);
    // The read threw on the pool thread.
    [[nodiscard]] static response_file_failure read_threw(std::exception_ptr exception) noexcept;

    // The error a response whose head was not committed is answered with:
    // 503 file_read_unavailable when the read never completed, otherwise 500
    // file_read_failed. Neither names the file or the system error.
    [[nodiscard]] std::exception_ptr response_error() const;

    // The fault for the connection failure channel, or null for an expected
    // outcome: a file removed, replaced or changed since it was framed, a
    // full pool queue (counted by blocking_pool_stats), or shutdown.
    [[nodiscard]] const std::exception_ptr& fault() const& noexcept RUVIA_LIFETIMEBOUND {
        return fault_;
    }
    const std::exception_ptr& fault() const&& = delete;

private:
    response_file_failure(bool unavailable, std::exception_ptr fault) noexcept :
        fault_(std::move(fault)),
        unavailable_(unavailable) {}

    std::exception_ptr fault_;
    bool unavailable_;
};

class response_body_chunk final {
public:
    enum class kind_type : std::uint8_t {
        data,
        end,
        failed,
    };

    [[nodiscard]] static constexpr response_body_chunk data(std::string_view bytes) noexcept {
        return response_body_chunk(kind_type::data, bytes);
    }

    [[nodiscard]] static constexpr response_body_chunk end() noexcept {
        return response_body_chunk(kind_type::end, {});
    }

    [[nodiscard]] static constexpr response_body_chunk failed() noexcept {
        return response_body_chunk(kind_type::failed, {});
    }

    [[nodiscard]] constexpr kind_type kind() const noexcept {
        return kind_;
    }

    // Valid until the next call to response_file_source::next() or
    // release_buffer().
    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }

private:
    constexpr response_body_chunk(kind_type kind, std::string_view bytes) noexcept :
        bytes_(bytes),
        kind_(kind) {}

    std::string_view bytes_;
    kind_type kind_;
};

class response_file_source final {
public:
    // `response` is borrowed until the last chunk was consumed.
    response_file_source(const http_response& response, response_file_io io);

    response_file_source(const response_file_source&) = delete;
    response_file_source& operator=(const response_file_source&) = delete;
    response_file_source(response_file_source&&) = delete;
    response_file_source& operator=(response_file_source&&) = delete;

    // Opens and validates the first file range without reading it or
    // allocating a read buffer: one pool round trip that lets a transport
    // commit its head before any flow-control credit exists. Idempotent.
    [[nodiscard]] task<bool> open_first();

    // Yields the body in order: in-memory segments as borrowed views, file
    // ranges as read-ahead buffers of at most `max_bytes` (nonzero). `failed`
    // means a file could not be opened or read, ended early, changed
    // identity, or the read did not complete; failure() says why. Once the
    // head is committed the connection (HTTP/1) or stream (HTTP/2) must end.
    [[nodiscard]] task<response_body_chunk> next(std::size_t max_bytes = response_file_read_ahead_bytes);

    // Returns the read buffer once the last chunk's bytes were consumed, so a
    // body waiting for flow-control credit holds none; the next file chunk
    // allocates again.
    void release_buffer() noexcept;

    // Whether the next chunk next() yields will be read from a file, so a
    // flow-controlled transport can wait for credit before reading it.
    [[nodiscard]] bool file_bytes_pending() const noexcept;

    [[nodiscard]] const response_file_failure* failure() const& noexcept RUVIA_LIFETIMEBOUND {
        return failure_ ? &*failure_ : nullptr;
    }
    const response_file_failure* failure() const&& = delete;

private:
    // Owned by exactly one thread at a time: it moves into the blocking
    // callable and back, so an abandoned read frees it on the pool thread
    // through the synchronized process resource.
    class read_buffer final {
    public:
        read_buffer() noexcept = default;

        [[nodiscard]] char* data() & RUVIA_LIFETIMEBOUND;
        char* data() && = delete;
        void reset() noexcept;

    private:
        struct buffer_deleter {
            void operator()(char* bytes) const noexcept;
        };

        std::unique_ptr<char, buffer_deleter> data_;
    };

    struct read_state final {
        read_state();

        http_native_path_string_type path_;
        std::uint64_t size_{};
        http_response_file_identity identity_{http_response_file_identity::unchecked()};
        // A duplicate of the response's handle, or empty until the pool opens
        // path_ for a body framed without one.
        native_file_descriptor input_;
        read_buffer buffer_;
        std::uint64_t position_{};
        std::size_t filled_{};
        std::error_code error_{};
        // input_ matched identity_ and size_ when it was first used.
        bool validated_{false};
#if defined(__linux__)
        bool cached_read_supported_{true};
#endif
    };

    static void read_ahead(read_state& state, std::size_t bytes, bool completes_range);
    [[nodiscard]] task<bool> fill(std::size_t bytes, bool completes_range);
    [[nodiscard]] bool settle_read();
    void select_file(http_response_file_view file);

    const http_response& response_;
    response_file_io io_;
    read_state state_;
    std::optional<response_file_failure> failure_;
    const http_native_path_char_type* current_path_{};
    std::uint64_t range_remaining_{};
    std::size_t next_segment_{};
    std::size_t segment_count_{};
    bool first_file_opened_{false};
};

}  // namespace ruvia::detail
