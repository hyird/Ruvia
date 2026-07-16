#pragma once

#include "ruvia/http/detail/NativePath.h"

#include <cstdint>
#include <filesystem>
#include <array>

namespace ruvia::detail {

// Opaque runtime-supplied identity of the file whose metadata was used to
// frame a response. The sans-I/O HTTP layer only transports this token; the
// web runtime defines and validates its platform-specific words after opening
// the actual file handle.
class ResponseFileIdentity final {
public:
    [[nodiscard]] static constexpr ResponseFileIdentity unchecked() noexcept {
        return ResponseFileIdentity({}, false);
    }

    [[nodiscard]] static constexpr ResponseFileIdentity checked(
        std::array<std::uint64_t, 4> words) noexcept {
        return ResponseFileIdentity(words, true);
    }

    [[nodiscard]] constexpr bool requiresValidation() const noexcept {
        return checked_;
    }

    [[nodiscard]] constexpr const std::array<std::uint64_t, 4>& words()
        const noexcept {
        return words_;
    }

    friend constexpr bool operator==(
        const ResponseFileIdentity&,
        const ResponseFileIdentity&) noexcept = default;

private:
    constexpr ResponseFileIdentity(
        std::array<std::uint64_t, 4> words,
        bool checked) noexcept
        : words_(words), checked_(checked) {}

    std::array<std::uint64_t, 4> words_{};
    bool checked_{false};
};

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

    [[nodiscard]] constexpr ResponseFileIdentity identity() const noexcept {
        return identity_;
    }

private:
    friend class HttpResponseBody;

    constexpr ResponseFileBody(
        const HttpNativePathChar* nativePath,
        std::uint64_t size,
        std::uint64_t offset,
        std::uint64_t length,
        ResponseFileIdentity identity) noexcept
        : nativePath_(nativePath),
          size_(size),
          offset_(offset),
          length_(length),
          identity_(identity) {}

    const HttpNativePathChar* nativePath_;
    std::uint64_t size_;
    std::uint64_t offset_;
    std::uint64_t length_;
    ResponseFileIdentity identity_;
};

}  // namespace ruvia::detail
