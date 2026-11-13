#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_request_content_semantics.h"
namespace ruvia {

// A byte range already resolved against the selected representation.
struct http_byte_range final {
    std::uint64_t offset_{};
    std::uint64_t length_{};

    [[nodiscard]] constexpr std::uint64_t offset() const noexcept {
        return offset_;
    }

    [[nodiscard]] constexpr std::uint64_t length() const noexcept {
        return length_;
    }
};
enum class http_byte_range_order_policy : std::uint8_t {
    coalesce,
    strict_ascending_disjoint,
};

struct http_byte_range_options final {
    std::size_t max_ranges_{16};
    http_byte_range_order_policy order_policy_{http_byte_range_order_policy::coalesce};
    bool strict_order_{false};
};

// Bounded result for a byte-range set. Inline storage covers up to 16 ranges
// without allocation; larger configured max_ranges_ spill into heap storage.
class http_byte_range_set final {
public:
    static constexpr std::size_t capacity = 16;

    using range = http_byte_range;

    http_byte_range_set() noexcept = default;

    [[nodiscard]] constexpr bool ignored() const noexcept {
        return ignored_;
    }
    [[nodiscard]] constexpr bool unsatisfiable() const noexcept {
        return !ignored_ && count_ == 0;
    }
    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return count_;
    }
    [[nodiscard]] std::span<const range> ranges() const& noexcept RUVIA_LIFETIMEBOUND {
        return overflow_.empty() ? std::span<const range>(ranges_.data(), count_)
                                 : std::span<const range>(overflow_.data(), count_);
    }
    std::span<const range> ranges() const&& = delete;

    [[nodiscard]] const range* begin() const& noexcept RUVIA_LIFETIMEBOUND {
        return ranges().data();
    }
    const range* begin() const&& = delete;

    [[nodiscard]] const range* end() const& noexcept RUVIA_LIFETIMEBOUND {
        return ranges().data() + count_;
    }
    const range* end() const&& = delete;

    // Throws std::out_of_range for index >= size().
    [[nodiscard]] const range& operator[](std::size_t index) const& RUVIA_LIFETIMEBOUND {
        if (index >= count_) {
            throw std::out_of_range("HTTP byte range index out of range");
        }
        return overflow_.empty() ? ranges_[index] : overflow_[index];
    }
    const range& operator[](std::size_t) const&& = delete;

private:
    friend http_byte_range_set resolve_http_byte_range_set(
        std::string_view, http_content_length, http_byte_range_options);

    range& mutable_at(std::size_t index) noexcept {
        return overflow_.empty() ? ranges_[index] : overflow_[index];
    }

    void append_slot(range value) {
        if (overflow_.empty() && count_ < capacity) {
            ranges_[count_++] = value;
            return;
        }
        if (overflow_.empty()) {
            overflow_.reserve(capacity * 2);
            overflow_.insert(overflow_.end(), ranges_.begin(), ranges_.begin() + count_);
        }
        overflow_.push_back(value);
        count_ = overflow_.size();
    }

    void resize_slots(std::size_t new_count) noexcept {
        count_ = new_count;
        if (!overflow_.empty()) {
            overflow_.resize(new_count);
        }
    }

    std::array<range, capacity> ranges_{};
    std::vector<range> overflow_;
    std::size_t count_{};
    bool ignored_{true};
};

[[nodiscard]] http_byte_range_set resolve_http_byte_range_set(
    std::string_view field_value, http_content_length representation_length_value,
    http_byte_range_options options);

[[nodiscard]] inline http_byte_range_set resolve_http_byte_range_set(
    std::string_view field_value, http_content_length representation_length_value) noexcept {
    return resolve_http_byte_range_set(field_value, representation_length_value, {});
}

// Parsed Content-Range: "bytes first-last/total" (RFC 9110 section 14.4).
struct http_content_range final {
    std::uint64_t first_{};
    std::uint64_t last_{};
    std::uint64_t total_{};

    [[nodiscard]] constexpr std::uint64_t first() const noexcept {
        return first_;
    }
    [[nodiscard]] constexpr std::uint64_t last() const noexcept {
        return last_;
    }
    [[nodiscard]] constexpr std::uint64_t total() const noexcept {
        return total_;
    }
    [[nodiscard]] constexpr std::uint64_t offset() const noexcept {
        return first_;
    }
    [[nodiscard]] constexpr std::uint64_t length() const noexcept {
        return last_ - first_ + 1;
    }
    [[nodiscard]] constexpr http_byte_range range() const noexcept {
        return {first_, last_ - first_ + 1};
    }

    friend constexpr bool operator==(const http_content_range&, const http_content_range&) noexcept = default;
};

[[nodiscard]] std::optional<http_content_range> parse_http_content_range(
    std::string_view field_value) noexcept;

[[nodiscard]] std::optional<std::uint64_t> compute_http_byte_range_content_length(
    std::span<const http_byte_range> ranges, http_content_length representation_length,
    std::string_view media_type = {}, std::string_view boundary = {}) noexcept;
[[nodiscard]] std::optional<std::uint64_t> compute_http_byte_range_content_length(
    const http_byte_range_set& ranges, http_content_length representation_length,
    std::string_view media_type = {}, std::string_view boundary = {}) noexcept;

// Streaming byte-range slicer for single-range 206 bodies and multi-range
// multipart/byteranges streams. Accepts sequential representation chunks (or
// partial chunks starting at a specified representation offset) and appends the
// framed output bytes to the caller's buffer.
class http_byte_range_slicer final {
public:
    http_byte_range_slicer(const http_byte_range_set& ranges,
        http_content_length representation_length, std::string_view media_type = {},
        std::string_view boundary = {});
    http_byte_range_slicer(std::span<const http_byte_range> ranges,
        http_content_length representation_length, std::string_view media_type = {},
        std::string_view boundary = {});

    [[nodiscard]] bool multipart() const noexcept {
        return ranges_.size() > 1;
    }
    [[nodiscard]] std::uint64_t content_length() const noexcept {
        return content_length_;
    }
    [[nodiscard]] std::string_view content_type() const& noexcept RUVIA_LIFETIMEBOUND {
        return content_type_;
    }
    std::string_view content_type() const&& = delete;
    [[nodiscard]] std::string_view content_range() const& noexcept RUVIA_LIFETIMEBOUND {
        return content_range_;
    }
    std::string_view content_range() const&& = delete;
    [[nodiscard]] std::uint64_t current_offset() const noexcept {
        return current_offset_;
    }
    [[nodiscard]] bool complete() const noexcept {
        return complete_;
    }

    void write(std::string_view chunk, std::string& output);
    void write(std::uint64_t chunk_offset, std::string_view chunk, std::string& output);
    void finish(std::string& output);
    void reset(std::uint64_t start_offset = 0) noexcept;

private:
    void emit_part_header(std::size_t index, std::string& output) const;

    std::vector<http_byte_range> ranges_;
    std::string media_type_;
    std::string boundary_;
    std::string content_type_;
    std::string content_range_;
    std::uint64_t representation_length_{0};
    std::uint64_t content_length_{0};
    std::uint64_t current_offset_{0};
    std::uint64_t range_progress_{0};
    std::size_t range_index_{0};
    bool complete_{false};
};

}  // namespace ruvia
