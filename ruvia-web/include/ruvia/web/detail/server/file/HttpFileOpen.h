#pragma once

// Owning, identity-validating file input for buffered web-layer drivers. The
// identity is checked on the same native handle that supplies response bytes,
// closing the stat/open replacement window.

#include "ruvia/http/detail/response/HttpResponseFileBody.h"
#include "ruvia/web/detail/server/file/HttpNativeFile.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <ios>
#include <limits>
#include <system_error>

namespace ruvia::detail {

#if defined(__unix__) || defined(__APPLE__) || defined(_WIN32)
class ResponseFileInput final {
public:
    explicit ResponseFileInput(ResponseFileBody file) noexcept {
        handle_ = openNativeFileForRead(file, error_);
    }

    ResponseFileInput(const ResponseFileInput&) = delete;
    ResponseFileInput& operator=(const ResponseFileInput&) = delete;
    ResponseFileInput(ResponseFileInput&&) = default;
    ResponseFileInput& operator=(ResponseFileInput&&) = default;

    explicit operator bool() const noexcept {
        return !error_;
    }

    void seekg(std::streamoff offset, std::ios::seekdir direction) noexcept {
        if (error_ || direction != std::ios::beg || offset < 0) {
            error_ = std::make_error_code(std::errc::invalid_seek);
            return;
        }
#if defined(__unix__) || defined(__APPLE__)
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
        lastRead_ = 0;
        if (error_ || size < 0) {
            error_ = std::make_error_code(std::errc::io_error);
            return;
        }
#if defined(__unix__) || defined(__APPLE__)
        const auto result = ::read(handle_.get(), output, static_cast<std::size_t>(size));
        if (result < 0) {
            error_ = std::error_code(errno, std::system_category());
            return;
        }
        lastRead_ = static_cast<std::streamsize>(result);
#else
        const auto requested = static_cast<DWORD>(std::min<std::uint64_t>(static_cast<std::uint64_t>(size), static_cast<std::uint64_t>((std::numeric_limits<DWORD>::max)())));
        DWORD result = 0;
        if (::ReadFile(handle_.get(), output, requested, &result, nullptr) == 0) {
            error_ = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
            return;
        }
        lastRead_ = static_cast<std::streamsize>(result);
#endif
    }

    [[nodiscard]] std::streamsize gcount() const noexcept {
        return lastRead_;
    }

private:
    NativeFileHandle handle_;
    std::error_code error_;
    std::streamsize lastRead_{0};
};
#else
class ResponseFileInput final {
public:
    explicit ResponseFileInput(ResponseFileBody file)
        : input_(file.toPath(), std::ios::binary) {
        if (!input_ || !file.identity().requiresValidation()) {
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

    void seekg(std::streamoff offset, std::ios::seekdir direction) {
        input_.seekg(offset, direction);
    }

    void read(char* output, std::streamsize size) {
        input_.read(output, size);
    }

    [[nodiscard]] std::streamsize gcount() const noexcept {
        return input_.gcount();
    }

private:
    std::ifstream input_;
};
#endif

[[nodiscard]] inline ResponseFileInput openResponseFileInput(ResponseFileBody file) {
    return ResponseFileInput(file);
}

}  // namespace ruvia::detail
