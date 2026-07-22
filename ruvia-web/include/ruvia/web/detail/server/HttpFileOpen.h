#pragma once

// Owning, identity-validating file input for buffered web-layer drivers. The
// identity is checked on the same native handle that supplies response bytes,
// closing the stat/open replacement window.

#include "ruvia/http/detail/HttpResponseFileBody.h"
#include "ruvia/web/detail/server/HttpNativeFile.h"

#include <cstddef>
#include <ios>
#include <system_error>

namespace ruvia::detail {

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
        if (::lseek(handle_.get(), static_cast<off_t>(offset), SEEK_SET) < 0) {
            error_ = std::error_code(errno, std::system_category());
        }
    }

    void read(char* output, std::streamsize size) noexcept {
        lastRead_ = 0;
        if (error_ || size < 0) {
            error_ = std::make_error_code(std::errc::io_error);
            return;
        }
        const auto result = ::read(
            handle_.get(), output, static_cast<std::size_t>(size));
        if (result < 0) {
            error_ = std::error_code(errno, std::system_category());
            return;
        }
        lastRead_ = static_cast<std::streamsize>(result);
    }

    [[nodiscard]] std::streamsize gcount() const noexcept {
        return lastRead_;
    }

private:
    NativeFileHandle handle_;
    std::error_code error_;
    std::streamsize lastRead_{0};
};

[[nodiscard]] inline ResponseFileInput openResponseFileInput(
    ResponseFileBody file) {
    return ResponseFileInput(file);
}

}  // namespace ruvia::detail
