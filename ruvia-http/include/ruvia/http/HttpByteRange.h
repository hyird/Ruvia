#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <string_view>
#include <system_error>

#include "ruvia/http/HttpAscii.h"
#include "ruvia/http/HttpFieldWhitespace.h"

namespace ruvia {

class HttpByteRangeUnsatisfiable final {
private:
    friend class http_byte_range_set;

    constexpr HttpByteRangeUnsatisfiable() noexcept = default;
};

// A byte range already resolved against the selected representation.
class HttpResolvedByteRange final {
public:
    [[nodiscard]] constexpr std::uint64_t offset() const noexcept {
        return offset_;
    }

    [[nodiscard]] constexpr std::uint64_t length() const noexcept {
        return length_;
    }

private:
    friend class http_byte_range_set;

    constexpr HttpResolvedByteRange(std::uint64_t offset, std::uint64_t length) noexcept
        : offset_(offset),
          length_(length) {
        if (length_ == 0 || offset_ > (std::numeric_limits<std::uint64_t>::max)() - length_) {
            std::terminate();
        }
    }

    std::uint64_t offset_;
    std::uint64_t length_;
};

// Bounded, allocation-free result for a byte-range set. The limit protects
// request planning and multipart framing from adversarial range counts.
class http_byte_range_set final {
public:
    static constexpr std::size_t capacity = 16;

    struct range final {
        std::uint64_t offset_{};
        std::uint64_t length_{};
    };

    [[nodiscard]] constexpr bool ignored() const noexcept {
        return ignored_;
    }
    [[nodiscard]] constexpr bool unsatisfiable() const noexcept {
        return !ignored_ && count_ == 0;
    }
    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return count_;
    }
    [[nodiscard]] constexpr const range& operator[](std::size_t index) const noexcept {
        return ranges_[index];
    }
    [[nodiscard]] constexpr HttpResolvedByteRange resolved_range(std::size_t index) const noexcept {
        return HttpResolvedByteRange(ranges_[index].offset_, ranges_[index].length_);
    }
    [[nodiscard]] constexpr HttpByteRangeUnsatisfiable unsatisfiable_outcome() const noexcept {
        return HttpByteRangeUnsatisfiable{};
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
    field_value = httpTrimOws(field_value);
    constexpr std::string_view unit = "bytes";
    if (field_value.size() <= unit.size() || field_value[unit.size()] != '=' ||
        !httpAsciiEqualsIgnoreCase(field_value.substr(0, unit.size()), unit)) {
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
    auto specs = httpTrimOws(field_value.substr(unit.size() + 1));
    if (specs.empty()) {
        return result;
    }
    result.ignored_ = false;
    std::size_t empty_members = 0;
    bool saw_range_spec = false;
    for (;;) {
        const auto comma = specs.find(',');
        auto spec = httpTrimOws(specs.substr(0, comma));
        const bool has_more = comma != std::string_view::npos;
        specs = has_more ? specs.substr(comma + 1) : std::string_view{};
        if (spec.empty()) {
            // RFC 9110 list recipients tolerate empty members; bound the work
            // while accepting more than the five elements RFC 9110 recommends.
            if (++empty_members > 32) {
                return http_byte_range_set{};
            }
            if (!has_more) {
                break;
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
            const auto resolved = http_byte_range_set::range{representation_length - length, length};
            if (result.count_ != 0) {
                auto& previous = result.ranges_[result.count_ - 1];
                const auto previous_end = previous.offset_ + previous.length_;
                if (resolved.offset_ >= previous.offset_ && resolved.offset_ <= previous_end) {
                    previous.length_ = std::max(previous_end, resolved.offset_ + resolved.length_) -
                                       previous.offset_;
                    if (!has_more) {
                        break;
                    }
                    continue;
                }
            }
            result.ranges_[result.count_++] = resolved;
            if (!has_more) {
                break;
            }
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
        const http_byte_range_set::range resolved{start, clamped_end - start + 1};
        // Preserve received part order (RFC 9110 §14.1.2). Coalesce only
        // ascending adjacent/overlapping neighbors; never reorder disjoint parts.
        if (result.count_ != 0) {
            auto& previous = result.ranges_[result.count_ - 1];
            const auto previous_end = previous.offset_ + previous.length_;
            if (resolved.offset_ >= previous.offset_ && resolved.offset_ <= previous_end) {
                previous.length_ = std::max(previous_end, resolved.offset_ + resolved.length_) -
                                   previous.offset_;
                if (!has_more) {
                    break;
                }
                continue;
            }
        }
        result.ranges_[result.count_++] = resolved;
        if (!has_more) {
            break;
        }
    }
    if (result.count_ == 0 && !saw_range_spec) {
        return http_byte_range_set{};
    }
    return result;
}

}  // namespace ruvia
