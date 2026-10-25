#pragma once

// Owning, identity-validating file input behind response_file_source. The
// identity is checked on the same native handle that supplies response bytes,
// closing the stat/open replacement window.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <limits>
#include <system_error>
#include <utility>

#if defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#endif

#include "ruvia/http/http_response_file.h"

#include "server/http_native_file.h"

namespace ruvia::detail {

#if defined(__unix__) || defined(_WIN32)
class response_file_input final {
public:
    explicit response_file_input(http_response_file_view file) noexcept {
        handle_ = open_native_file_for_read(file, error_);
    }

#if defined(__linux__)
    response_file_input(native_file_handle handle, http_response_file_view file) noexcept
        : handle_(std::move(handle)),
          error_(validate_native_file(handle_.get(), file)) {}

    // 只尝试已缓存的字节；未读完的后缀由同一读取器交给阻塞池。
    [[nodiscard]] std::size_t read_cached(std::uint64_t offset, char* output, std::size_t size) noexcept {
#if defined(SYS_preadv2) && defined(RWF_NOWAIT)
        if (!cached_read_supported_ || offset > static_cast<std::uint64_t>((std::numeric_limits<off_t>::max)())) {
            return 0;
        }
        iovec buffer{output, size};
        // 内核 ABI 按 unsigned long 拆分偏移；64 位 ABI 的高位参数为零。
        const auto low = static_cast<unsigned long>(offset);
        unsigned long high = 0;
        if constexpr (sizeof(unsigned long) < sizeof(offset)) {
            high = static_cast<unsigned long>(offset >> 32);
        }
        const auto count = ::syscall(SYS_preadv2, handle_.get(), &buffer, 1UL, low, high, RWF_NOWAIT);
        if (count < 0) {
            if (errno == ENOSYS || errno == EOPNOTSUPP || errno == EINVAL || errno == EPERM) {
                cached_read_supported_ = false;
            }
            return 0;
        }
        return static_cast<std::size_t>(count);
#else
        static_cast<void>(offset);
        static_cast<void>(output);
        static_cast<void>(size);
        return 0;
#endif
    }
#endif

    response_file_input(const response_file_input&) = delete;
    response_file_input& operator=(const response_file_input&) = delete;
    response_file_input(response_file_input&&) = default;
    response_file_input& operator=(response_file_input&&) = default;

    explicit operator bool() const noexcept {
        return !error_;
    }

    void seekg(std::streamoff offset) noexcept {
        if (error_ || offset < 0) {
            error_ = std::make_error_code(std::errc::invalid_seek);
            return;
        }
#if defined(__unix__)
        if (::lseek(handle_.get(), static_cast<off_t>(offset), SEEK_SET) < 0) {
            error_ = std::error_code(errno, std::system_category());
        }
#else
        LARGE_INTEGER position;
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (::SetFilePointerEx(handle_.get(), position, nullptr, FILE_BEGIN) == 0) {
            error_ = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
        }
#endif
    }

    void read(char* output, std::streamsize size) noexcept {
        last_read_ = 0;
        if (error_ || size < 0) {
            error_ = std::make_error_code(std::errc::io_error);
            return;
        }
#if defined(__unix__)
        ssize_t result_value = 0;
        do {
            result_value = ::read(handle_.get(), output, static_cast<std::size_t>(size));
        } while (result_value < 0 && errno == EINTR);
        if (result_value < 0) {
            error_ = std::error_code(errno, std::system_category());
            return;
        }
        last_read_ = static_cast<std::streamsize>(result_value);
#else
        const auto requested =
            static_cast<DWORD>(std::min<std::uint64_t>(static_cast<std::uint64_t>(size),
                static_cast<std::uint64_t>((std::numeric_limits<DWORD>::max)())));
        DWORD result_value = 0;
        if (::ReadFile(handle_.get(), output, requested, &result_value, nullptr) == 0) {
            error_ = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
            return;
        }
        last_read_ = static_cast<std::streamsize>(result_value);
#endif
    }

    [[nodiscard]] std::streamsize gcount() const noexcept {
        return last_read_;
    }

    // Recheck the same native handle after a whole-file operation. Opening a
    // descriptor validates replacement races, but an in-place write can alter
    // bytes while that descriptor remains valid. The ctime/change-time token
    // in http_response_file_identity makes that mutation visible to the runtime.
    [[nodiscard]] bool matches_snapshot(
        http_response_file_identity expected, std::uint64_t expected_size) const noexcept {
        if (!expected.requires_validation()) {
            return true;
        }
        std::error_code error;
        const auto snapshot = snapshot_native_file_handle(handle_.get(), error);
        return !error && snapshot.identity_ == expected && snapshot.size_ == expected_size;
    }

private:
    native_file_handle handle_;
    std::error_code error_;
    std::streamsize last_read_{0};
#if defined(__linux__) && defined(SYS_preadv2) && defined(RWF_NOWAIT)
    bool cached_read_supported_{true};
#endif
};
#else
class response_file_input final {
public:
    explicit response_file_input(http_response_file_view file)
        : input_(file.to_path(), std::ios::binary) {
        if (!input_ || !file.identity().requires_validation()) {
            return;
        }
        // No portable standard-library API exposes the native handle owned by
        // std::ifstream. Checked descriptors therefore fail closed instead of
        // validating one path lookup and reading from another.
        input_.setstate(std::ios::badbit);
    }

    explicit operator bool() const noexcept {
        return static_cast<bool>(input_);
    }

    void seekg(std::streamoff offset) {
        input_.seekg(offset, std::ios::beg);
    }

    void read(char* output, std::streamsize size) {
        input_.read(output, size);
    }

    [[nodiscard]] std::streamsize gcount() const noexcept {
        return input_.gcount();
    }

    [[nodiscard]] bool matches_snapshot(
        http_response_file_identity expected, std::uint64_t) const noexcept {
        // Checked identities are rejected by the portable fallback at open:
        // there is no native handle to validate without reopening a path.
        return !expected.requires_validation();
    }

private:
    std::ifstream input_;
};
#endif

// Fills output from the input's current position through the same handle that
// matches_snapshot() later validates; false on a read error or early EOF.
[[nodiscard]] inline bool read_exact(response_file_input& input, char* output, std::size_t size) {
    constexpr auto max_chunk = static_cast<std::size_t>((std::numeric_limits<std::streamsize>::max)());
    while (size != 0) {
        input.read(output, static_cast<std::streamsize>(std::min(size, max_chunk)));
        const auto count = input.gcount();
        if (!input || count <= 0) {
            return false;
        }
        output += count;
        size -= static_cast<std::size_t>(count);
    }
    return true;
}

}  // namespace ruvia::detail
