#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/detail/util/pmr_resource.h"
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
    std::pmr::memory_resource* resource_{nullptr};
};

// Bounded result for a byte-range set. Inline storage covers up to 16 ranges
// without allocation; larger configured max_ranges_ spill into PMR storage.
class http_byte_range_set final {
public:
    static constexpr std::size_t capacity = 16;

    using range = http_byte_range;

    http_byte_range_set() noexcept
        : overflow_(detail::http_pmr_resource_or_default(nullptr)) {}

    explicit http_byte_range_set(std::pmr::memory_resource* resource) noexcept
        : overflow_(detail::http_pmr_resource_or_default(resource)) {}

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
        std::string_view, http_content_length) noexcept;
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
    std::pmr::vector<range> overflow_;
    std::size_t count_{};
    bool ignored_{true};
};

[[nodiscard]] http_byte_range_set resolve_http_byte_range_set(
    std::string_view field_value, http_content_length representation_length_value,
    http_byte_range_options options);

[[nodiscard]] inline http_byte_range_set resolve_http_byte_range_set(
    std::string_view field_value, http_content_length representation_length_value) noexcept {
    const auto representation_length = representation_length_value.value();
    http_byte_range_set result;
    field_value = http_trim_ows(field_value);
    constexpr std::string_view unit = "bytes";
    if (field_value.size() <= unit.size() || field_value[unit.size()] != '=' ||
        !http_ascii_equals_ignore_case(field_value.substr(0, unit.size()), unit)) {
        return result;
    }
    if (representation_length == 0) {
        return result;
    }

    const auto parse_number = [](std::string_view digits, std::uint64_t& value) noexcept {
        if (digits.empty()) {
            return false;
        }
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (end != digits.data() + digits.size()) {
            return false;
        }
        if (error == std::errc::result_out_of_range) {
            value = (std::numeric_limits<std::uint64_t>::max)();
            return true;
        }
        return error == std::errc{};
    };
    // RFC 9110 sections 14.2 and 15.3.7.2 let a server coalesce overlapping or
    // adjacent ranges regardless of their received order. Doing so for every
    // pair keeps parts disjoint, so a response never repeats representation
    // bytes. Disjoint parts keep their received order (RFC 9110 section
    // 14.1.2); a coalesced part takes the place of its earliest member.
    const auto add_range = [&result](std::uint64_t offset, std::uint64_t length) noexcept {
        auto start = offset;
        auto end = offset + length;
        auto target = result.count_;
        std::size_t kept = 0;
        for (std::size_t index = 0; index < result.count_; ++index) {
            const auto current = result.ranges_[index];
            const auto current_end = current.offset_ + current.length_;
            if (current.offset_ <= end && start <= current_end) {
                // Existing parts never touch each other, so one pass reaches
                // every part the growing union can touch.
                start = std::min(start, current.offset_);
                end = std::max(end, current_end);
                if (target == result.count_) {
                    target = kept++;
                }
                continue;
            }
            result.ranges_[kept++] = current;
        }
        if (target == result.count_) {
            target = kept++;
        }
        result.ranges_[target] = http_byte_range_set::range{start, end - start};
        result.count_ = kept;
    };
    auto specs = http_trim_ows(field_value.substr(unit.size() + 1));
    if (specs.empty()) {
        return result;
    }
    result.ignored_ = false;
    std::size_t empty_members = 0;
    bool saw_range_spec = false;
    bool has_more = false;
    do {
        const auto comma = specs.find(',');
        auto spec = http_trim_ows(specs.substr(0, comma));
        has_more = comma != std::string_view::npos;
        specs = has_more ? specs.substr(comma + 1) : std::string_view{};
        if (spec.empty()) {
            if (++empty_members > 32) {
                return http_byte_range_set{};
            }
            continue;
        }
        saw_range_spec = true;
        if (result.count_ == http_byte_range_set::capacity) {
            return http_byte_range_set{};
        }
        const auto dash = spec.find('-');
        if (dash == std::string_view::npos || spec.find('-', dash + 1) != std::string_view::npos) {
            return http_byte_range_set{};
        }
        const auto first = spec.substr(0, dash);
        const auto last = spec.substr(dash + 1);
        std::uint64_t start{};
        std::uint64_t end{};
        if (first.empty()) {
            std::uint64_t suffix{};
            if (!parse_number(last, suffix)) {
                return http_byte_range_set{};
            }
            if (suffix == 0) {
                continue;
            }
            const auto length = std::min(suffix, representation_length);
            add_range(representation_length - length, length);
            continue;
        }
        if (!parse_number(first, start) || (!last.empty() && !parse_number(last, end))) {
            return http_byte_range_set{};
        }
        if (!last.empty()) {
            if (start > end) {
                return http_byte_range_set{};
            }
            if (start == (std::numeric_limits<std::uint64_t>::max)() && end == start) {
                auto normalized_first = first;
                auto normalized_last = last;
                while (normalized_first.size() > 1 && normalized_first.front() == '0') {
                    normalized_first.remove_prefix(1);
                }
                while (normalized_last.size() > 1 && normalized_last.front() == '0') {
                    normalized_last.remove_prefix(1);
                }
                if (normalized_first.size() > normalized_last.size() ||
                    (normalized_first.size() == normalized_last.size() && normalized_first > normalized_last)) {
                    return http_byte_range_set{};
                }
            }
        }
        if (start >= representation_length) {
            continue;
        }
        const auto clamped_end = last.empty() ? representation_length - 1
                                              : std::min(end, representation_length - 1);
        add_range(start, clamped_end - start + 1);
    } while (has_more);
    if (result.count_ == 0 && !saw_range_spec) {
        return http_byte_range_set{};
    }
    return result;
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
        std::string_view boundary = {}, std::pmr::memory_resource* resource = nullptr);
    http_byte_range_slicer(std::span<const http_byte_range> ranges,
        http_content_length representation_length, std::string_view media_type = {},
        std::string_view boundary = {}, std::pmr::memory_resource* resource = nullptr);

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

    void write(std::string_view chunk, std::pmr::string& output);
    void write(std::uint64_t chunk_offset, std::string_view chunk, std::pmr::string& output);
    void finish(std::pmr::string& output);
    void reset(std::uint64_t start_offset = 0) noexcept;

private:
    void emit_part_header(std::size_t index, std::pmr::string& output) const;

    std::pmr::memory_resource* resource_;
    std::pmr::vector<http_byte_range> ranges_;
    std::pmr::string media_type_;
    std::pmr::string boundary_;
    std::pmr::string content_type_;
    std::pmr::string content_range_;
    std::uint64_t representation_length_{0};
    std::uint64_t content_length_{0};
    std::uint64_t current_offset_{0};
    std::uint64_t range_progress_{0};
    std::size_t range_index_{0};
    bool complete_{false};
};

}  // namespace ruvia
