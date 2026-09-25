#pragma once

#include <array>
#include <cstdint>
#include <filesystem>

namespace ruvia {

// Opaque runtime-supplied identity of the file whose metadata was used to
// frame a response. HTTP transports this token; the runtime validates it after
// opening the actual file handle.
class HttpResponseFileIdentity final {
public:
    [[nodiscard]] static constexpr HttpResponseFileIdentity unchecked() noexcept {
        return HttpResponseFileIdentity({}, false);
    }

    [[nodiscard]] static constexpr HttpResponseFileIdentity checked(
        std::array<std::uint64_t, 4> words) noexcept {
        return HttpResponseFileIdentity(words, true);
    }

    [[nodiscard]] constexpr bool requiresValidation() const noexcept {
        return checked_;
    }
    [[nodiscard]] constexpr const std::array<std::uint64_t, 4>& words() const& noexcept {
        return words_;
    }
    [[nodiscard]] constexpr const std::array<std::uint64_t, 4>& words() const&& = delete;

    friend constexpr bool operator==(
        const HttpResponseFileIdentity&, const HttpResponseFileIdentity&) noexcept = default;

private:
    constexpr HttpResponseFileIdentity(std::array<std::uint64_t, 4> words, bool checked) noexcept
        : words_(words),
          checked_(checked) {}

    std::array<std::uint64_t, 4> words_{};
    bool checked_{false};
};

// Read-only descriptor for a response file. Its path is borrowed and remains
// valid for the lifetime of the response body from which it was obtained.
class HttpResponseFileView final {
public:
    using NativePathChar = std::filesystem::path::value_type;

    constexpr HttpResponseFileView(const NativePathChar* nativePath, std::uint64_t size,
        std::uint64_t offset, std::uint64_t length, HttpResponseFileIdentity identity) noexcept
        : nativePath_(nativePath),
          size_(size),
          offset_(offset),
          length_(length),
          identity_(identity) {}

    [[nodiscard]] constexpr const NativePathChar* nativePathCStr() const noexcept {
        return nativePath_;
    }
    [[nodiscard]] std::filesystem::path toPath() const {
        return std::filesystem::path(nativePath_);
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
    [[nodiscard]] constexpr HttpResponseFileIdentity identity() const noexcept {
        return identity_;
    }

private:
    const NativePathChar* nativePath_;
    std::uint64_t size_;
    std::uint64_t offset_;
    std::uint64_t length_;
    HttpResponseFileIdentity identity_;
};

}  // namespace ruvia

namespace ruvia::detail {
// Compatibility name retained for existing runtime integrations.
using ResponseFileIdentity = ruvia::HttpResponseFileIdentity;
}  // namespace ruvia::detail
