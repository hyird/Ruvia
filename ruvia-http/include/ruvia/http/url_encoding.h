#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_hex.h"

namespace ruvia {

enum class url_decode_mode : std::uint8_t { percent,
    form };

struct url_decode_options final {
    url_decode_mode mode_{url_decode_mode::percent};
    std::pmr::memory_resource* resource_{nullptr};
};

// Percent mode treats '+' literally; form mode decodes '+' as a space.
[[nodiscard]] inline bool has_url_encoding(std::string_view value, url_decode_mode mode) noexcept {
    // Keep short components and early escapes on the scalar path; library searches
    // can scan the remaining literal text in larger chunks.
    constexpr std::size_t scalar_scan_limit = 64;
    constexpr std::size_t scalar_prefix_size = 32;
    const auto prefix = value.substr(0, value.size() <= scalar_scan_limit ? value.size() : scalar_prefix_size);
    const bool encoded = mode == url_decode_mode::form
                             ? std::ranges::any_of(prefix, [](char c) noexcept { return c == '%' || c == '+'; })
                             : std::ranges::any_of(prefix, [](char c) noexcept { return c == '%'; });
    if (encoded) {
        return true;
    }
    if (prefix.size() == value.size()) {
        return false;
    }
    value.remove_prefix(prefix.size());
    return (mode == url_decode_mode::form ? value.find_first_of("%+") : value.find('%')) !=
           std::string_view::npos;
}

// Decodes the pct-encoded triplet starting at input[index] (RFC 3986 2.1):
// '%' followed by two hex digits. Returns no value when the triplet is
// truncated, does not start with '%', or contains a non-hex digit.
[[nodiscard]] constexpr std::optional<unsigned char> http_try_decode_percent_byte(
    std::string_view input, std::size_t index) noexcept {
    if (index >= input.size() || input.size() - index < 3 || input[index] != '%') {
        return std::nullopt;
    }
    const auto high = http_decode_hex_nibble(input[index + 1]);
    const auto low = http_decode_hex_nibble(input[index + 2]);
    if (high < 0 || low < 0) {
        return std::nullopt;
    }
    return static_cast<unsigned char>((high << 4) | low);
}

// Appends the canonical uppercase pct-encoded triplet for byte. Callers own the
// policy deciding which bytes need escaping.
template <typename output_type>
void http_append_percent_byte(output_type& output, unsigned char byte) {
    output.push_back('%');
    output.push_back(http_upper_hex_digit(byte >> 4));
    output.push_back(http_upper_hex_digit(byte));
}

namespace detail {
template <typename visitor_type>
[[nodiscard]] bool dispatch_url_encoded_pair_visitor(
    visitor_type& visitor, std::string_view name, std::string_view value) {
    if constexpr (requires {
                      { visitor(name, value) } -> std::convertible_to<bool>;
                  }) {
        return static_cast<bool>(visitor(name, value));
    } else {
        visitor(name, value);
        return true;
    }
}

}  // namespace detail

[[nodiscard]] inline bool validate_url_encoding(std::string_view value) noexcept {
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '%') {
            continue;
        }
        if (!http_try_decode_percent_byte(value, i)) {
            return false;
        }
        i += 2;
    }
    return true;
}

// Returns the complete decoded component or no value for malformed percent
// encoding. Percent mode treats '+' literally; form mode converts it to a space.
// The result is allocated from options.resource (or the default PMR resource),
// which must outlive the returned string. On failure, no decoded prefix escapes.
[[nodiscard]] inline std::optional<std::pmr::string> decode_url_component(
    std::string_view input, url_decode_options options = {}) {
    std::pmr::string output(detail::http_pmr_resource_or_default(options.resource_));
    output.resize(input.size());
    std::size_t written = 0;
    for (std::size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (options.mode_ == url_decode_mode::form && c == '+') {
            c = ' ';
        } else if (c == '%') {
            const auto byte = http_try_decode_percent_byte(input, i);
            if (!byte) {
                return std::nullopt;
            }
            c = static_cast<char>(*byte);
            i += 2;
        }
        output[written++] = c;
    }
    output.resize(written);
    return output;
}

// Compare two percent-encoded components by their decoded bytes without
// allocating, so differently escaped spellings of one value compare equal.
// Percent mode treats '+' literally; form mode converts it to a space.
// Malformed percent escapes never compare equal.
[[nodiscard]] inline bool url_components_equivalent(
    std::string_view left, std::string_view right, url_decode_mode mode) noexcept {
    const auto next_byte = [mode](std::string_view input, std::size_t& i) noexcept -> int {
        const char c = input[i];
        if (mode == url_decode_mode::form && c == '+') {
            return ' ';
        }
        if (c == '%') {
            const auto byte = http_try_decode_percent_byte(input, i);
            if (!byte) {
                return -1;
            }
            i += 2;
            return *byte;
        }
        return static_cast<unsigned char>(c);
    };
    std::size_t left_index = 0;
    std::size_t right_index = 0;
    for (; left_index < left.size() && right_index < right.size(); ++left_index, ++right_index) {
        const auto left_byte = next_byte(left, left_index);
        const auto right_byte = next_byte(right, right_index);
        if (left_byte < 0 || right_byte < 0 || left_byte != right_byte) {
            return false;
        }
    }
    return left_index == left.size() && right_index == right.size();
}

// Visit raw, borrowed name/value views for each non-empty pair, preserving repeated
// names in input order. A bool-returning visitor returning false stops iteration
// and makes this return false; a void visitor always traverses all pairs. Pair
// traversal does not validate percent escapes. Input storage must outlive each view.
template <typename visitor_type>
[[nodiscard]] bool visit_url_encoded_pairs(std::string_view input, visitor_type&& visitor) {
    auto& visitor_ref = visitor;
    while (!input.empty()) {
        const auto pair_end = input.find('&');
        const auto pair = pair_end == std::string_view::npos ? input : input.substr(0, pair_end);

        // Skip empty segments ("&&", leading/trailing "&") rather than emitting a phantom
        // ("", "") pair : a segment with no bytes carries no field.
        if (!pair.empty()) {
            const auto equals = pair.find('=');
            const auto name = equals == std::string_view::npos ? pair : pair.substr(0, equals);
            const auto value =
                equals == std::string_view::npos ? std::string_view{} : pair.substr(equals + 1);
            if (!detail::dispatch_url_encoded_pair_visitor(visitor_ref, name, value)) {
                return false;
            }
        }

        if (pair_end == std::string_view::npos) {
            return true;
        }
        input.remove_prefix(pair_end + 1);
    }
    return true;
}

}  // namespace ruvia
