#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_field_syntax.h"

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

// Bounded, allocation-free result for a byte-range set. The limit protects
// request planning and multipart framing from adversarial range counts.
class http_byte_range_set final {
public:
    static constexpr std::size_t capacity = 16;

    using range = http_byte_range;
    [[nodiscard]] constexpr bool ignored() const noexcept {
        return ignored_;
    }
    [[nodiscard]] constexpr bool unsatisfiable() const noexcept {
        return !ignored_ && count_ == 0;
    }
    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return count_;
    }
    // Both accessors throw std::out_of_range for index >= size().
    [[nodiscard]] constexpr const range& operator[](std::size_t index) const {
        if (index >= count_) {
            throw std::out_of_range("HTTP byte range index out of range");
        }
        return ranges_[index];
    }

private:
    friend http_byte_range_set resolve_http_byte_range_set(std::string_view, std::uint64_t) noexcept;

    std::array<range, capacity> ranges_{};
    std::size_t count_{};
    bool ignored_{true};
};

[[nodiscard]] inline http_byte_range_set resolve_http_byte_range_set(
    std::string_view field_value, std::uint64_t representation_length) noexcept {
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
            // RFC 9110 list recipients tolerate empty members; bound the work
            // while accepting more than the five elements RFC 9110 recommends.
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

}  // namespace ruvia
