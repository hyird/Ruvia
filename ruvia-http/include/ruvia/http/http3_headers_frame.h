#pragma once

#include <cstddef>
#include <memory_resource>
#include <span>
#include <utility>
#include <vector>

#include "ruvia/http/http3_var_int.h"

namespace ruvia::detail {
struct http3_headers_frame_access;
}

namespace ruvia {

// One complete HTTP/3 HEADERS frame (RFC 9114 section 7.2.2) owned by a PMR
// resource; every head and trailer encoder returns this type. Encoders reserve
// the largest frame header in front of the QPACK field section and write the
// actual header immediately before the section, so producing the wire frame
// never copies the section. The resource must outlive this value.
class http3_headers_frame final {
public:
    // A one-byte HEADERS type plus the largest length integer.
    static constexpr std::size_t reserved_prefix_bytes = 1 + http3_var_int_max_bytes;

    http3_headers_frame(const http3_headers_frame&) = delete;
    http3_headers_frame& operator=(const http3_headers_frame&) = delete;
    http3_headers_frame(http3_headers_frame&& other) noexcept
        : bytes_(std::move(other.bytes_)),
          frame_offset_(std::exchange(other.frame_offset_, 0)),
          decoded_field_section_size_(std::exchange(other.decoded_field_section_size_, 0)) {
        other.bytes_.clear();
    }
    http3_headers_frame& operator=(http3_headers_frame&& other) {
        if (this != &other) {
            if (bytes_.get_allocator() == other.bytes_.get_allocator()) {
                bytes_ = std::move(other.bytes_);
            } else {
                // Unequal PMR allocators require a copy. Allocate before
                // changing either frame so failure preserves bytes and metadata.
                std::pmr::vector<char> replacement(other.bytes_, bytes_.get_allocator());
                bytes_.swap(replacement);
            }
            frame_offset_ = std::exchange(other.frame_offset_, 0);
            decoded_field_section_size_ = std::exchange(other.decoded_field_section_size_, 0);
            other.bytes_.clear();
        }
        return *this;
    }

    // Frame type, length and field section: the exact request-stream bytes.
    [[nodiscard]] std::span<const char> frame() const& noexcept {
        return bytes_.empty() ? std::span<const char>{} : std::span<const char>(bytes_).subspan(frame_offset_);
    }
    std::span<const char> frame() const&& = delete;
    // The QPACK-encoded field section carried as the frame payload.
    [[nodiscard]] std::span<const char> field_section() const& noexcept {
        return bytes_.empty() ? std::span<const char>{} : std::span<const char>(bytes_).subspan(reserved_prefix_bytes);
    }
    std::span<const char> field_section() const&& = delete;
    // RFC 9114 field-list size from emitted names and values, not QPACK bytes.
    [[nodiscard]] std::size_t decoded_field_section_size() const noexcept {
        return decoded_field_section_size_;
    }
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return bytes_.get_allocator().resource();
    }

private:
    friend struct detail::http3_headers_frame_access;

    http3_headers_frame(std::pmr::vector<char> bytes_value, std::size_t frame_offset,
        std::size_t decoded_size) noexcept
        : bytes_(std::move(bytes_value)),
          frame_offset_(frame_offset),
          decoded_field_section_size_(decoded_size) {}

    std::pmr::vector<char> bytes_;
    std::size_t frame_offset_;
    std::size_t decoded_field_section_size_;
};

}  // namespace ruvia
