#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace ruvia {

// Opaque runtime-supplied identity of the file whose metadata was used to
// frame a response. HTTP transports this token; the runtime validates it after
// opening the actual file handle.
class http_response_file_identity final {
public:
    [[nodiscard]] static constexpr http_response_file_identity unchecked() noexcept {
        return http_response_file_identity({}, false);
    }

    [[nodiscard]] static constexpr http_response_file_identity checked(
        std::array<std::uint64_t, 4> words) noexcept {
        return http_response_file_identity(words, true);
    }

    [[nodiscard]] constexpr bool requires_validation() const noexcept {
        return checked_;
    }
    [[nodiscard]] constexpr const std::array<std::uint64_t, 4>& words() const& noexcept {
        return words_;
    }
    [[nodiscard]] constexpr const std::array<std::uint64_t, 4>& words() const&& = delete;

    friend constexpr bool operator==(
        const http_response_file_identity&, const http_response_file_identity&) noexcept = default;

private:
    constexpr http_response_file_identity(std::array<std::uint64_t, 4> words, bool checked) noexcept
        : words_(words),
          checked_(checked) {}

    std::array<std::uint64_t, 4> words_{};
    bool checked_{false};
};

// Read-only descriptor for a response file. Its path is borrowed and remains
// valid for the lifetime of the response body from which it was obtained.
class http_response_file_view final {
public:
    using native_path_char_type = std::filesystem::path::value_type;

    constexpr http_response_file_view(const native_path_char_type* native_path, std::uint64_t size,
        std::uint64_t offset, std::uint64_t length, http_response_file_identity identity) noexcept
        : native_path_(native_path),
          size_(size),
          offset_(offset),
          length_(length),
          identity_(identity) {}

    [[nodiscard]] constexpr const native_path_char_type* native_path_c_str() const noexcept {
        return native_path_;
    }
    [[nodiscard]] std::filesystem::path to_path() const {
        return std::filesystem::path(native_path_);
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
    [[nodiscard]] constexpr http_response_file_identity identity() const noexcept {
        return identity_;
    }

private:
    const native_path_char_type* native_path_;
    std::uint64_t size_;
    std::uint64_t offset_;
    std::uint64_t length_;
    http_response_file_identity identity_;
};

// A borrowed view of one response-body segment. Any bytes and file path remain
// valid only while the owning http_response is alive and unmodified.
struct http_response_body_segment_view final {
    std::string_view bytes_{};
    std::optional<http_response_file_view> file_{};
};

}  // namespace ruvia
