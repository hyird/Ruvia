#pragma once

#include "ruvia/http/detail/NativePath.h"

#include <cstdint>
#include <filesystem>

namespace ruvia::detail {

class HttpResponseBody;

// Non-owning descriptor derived atomically from an active response file
// alternative. Its native path remains valid until that response body is
// replaced or destroyed.
class ResponseFileBody final {
public:
    [[nodiscard]] constexpr const HttpNativePathChar* nativePathCStr()
        const noexcept {
        return nativePath_;
    }

    [[nodiscard]] std::filesystem::path toPath() const {
        return makePathFromHttpNativePath(nativePath_);
    }

    [[nodiscard]] constexpr std::uint64_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] constexpr std::uint64_t offset() const noexcept {
        return offset_;
    }

    [[nodiscard]] constexpr std::uint64_t length() const noexcept {
        return length_;
    }

private:
    friend class HttpResponseBody;

    constexpr ResponseFileBody(
        const HttpNativePathChar* nativePath,
        std::uint64_t size,
        std::uint64_t offset,
        std::uint64_t length) noexcept
        : nativePath_(nativePath),
          size_(size),
          offset_(offset),
          length_(length) {}

    const HttpNativePathChar* nativePath_;
    std::uint64_t size_;
    std::uint64_t offset_;
    std::uint64_t length_;
};

}  // namespace ruvia::detail
