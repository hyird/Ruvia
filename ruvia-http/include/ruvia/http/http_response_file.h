#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <utility>

#include "ruvia/http/http_native_path.h"

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

// An open native file a runtime already framed a response from, carried by
// the response body so the transport sends from that same handle instead of
// resolving the path again. HTTP performs no I/O on it: it stores the native
// value and invokes the runtime-supplied release function exactly once when
// the owning body is destroyed or replaced.
class http_response_file_handle final {
public:
    using native_type = std::uintptr_t;
    using release_type = void (*)(native_type) noexcept;

    constexpr http_response_file_handle() noexcept = default;
    constexpr http_response_file_handle(native_type native_value, release_type release) noexcept
        : native_(native_value),
          release_(release) {}

    http_response_file_handle(const http_response_file_handle&) = delete;
    http_response_file_handle& operator=(const http_response_file_handle&) = delete;

    constexpr http_response_file_handle(http_response_file_handle&& other) noexcept
        : native_(std::exchange(other.native_, native_type{})),
          release_(std::exchange(other.release_, nullptr)) {}

    http_response_file_handle& operator=(http_response_file_handle&& other) noexcept {
        if (this != &other) {
            reset();
            native_ = std::exchange(other.native_, native_type{});
            release_ = std::exchange(other.release_, nullptr);
        }
        return *this;
    }

    ~http_response_file_handle() {
        reset();
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return release_ != nullptr;
    }

    // Borrowed while this handle is alive; meaningful only when it is engaged.
    [[nodiscard]] constexpr native_type native() const noexcept {
        return native_;
    }

private:
    void reset() noexcept {
        if (release_ != nullptr) {
            std::exchange(release_, nullptr)(std::exchange(native_, native_type{}));
        }
    }

    native_type native_{};
    release_type release_{nullptr};
};

// Read-only descriptor for a response file. Its path and any opened handle are
// borrowed and remain valid for the lifetime of the response body from which
// it was obtained.
class http_response_file_view final {
public:
    constexpr http_response_file_view(const http_native_path_char_type* native_path, std::uint64_t size,
        std::uint64_t offset, std::uint64_t length, http_response_file_identity identity,
        const http_response_file_handle* opened = nullptr) noexcept
        : native_path_(native_path),
          opened_(opened != nullptr && *opened ? opened : nullptr),
          size_(size),
          offset_(offset),
          length_(length),
          identity_(identity) {}

    [[nodiscard]] constexpr const http_native_path_char_type* native_path_c_str() const noexcept {
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
    // The handle the response was framed from, or null when the transport
    // must open native_path_c_str() and validate identity() itself.
    [[nodiscard]] constexpr const http_response_file_handle* opened_handle() const noexcept {
        return opened_;
    }

private:
    const http_native_path_char_type* native_path_;
    const http_response_file_handle* opened_;
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
