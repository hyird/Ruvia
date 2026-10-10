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

#include "ruvia/http/detail/util/borrowed_view.h"
#include "ruvia/http/detail/util/hex.h"
#include "ruvia/http/detail/util/pmr_resource.h"

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

namespace detail {

// Decode the percent-escape at position i, where input[i] == '%'. Returns the
// decoded byte (0-255), or -1 if the escape is truncated or contains a non-hex
// digit. On success, advances i past the two hex digits so a `for (...; ++i)`
// loop lands on the next input character. Single owner of %XX decoding for the
// URL-component helpers below.
[[nodiscard]] inline int decode_percent_byte(std::string_view input, std::size_t& i) noexcept {
    if (i + 2 >= input.size()) {
        return -1;
    }
    const auto high = decode_hex_nibble(input[i + 1]);
    const auto low = decode_hex_nibble(input[i + 2]);
    if (high < 0 || low < 0) {
        return -1;
    }
    i += 2;
    return (high << 4) | low;
}

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
        if (detail::decode_percent_byte(value, i) < 0) {
            return false;
        }
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
            const int byte = detail::decode_percent_byte(input, i);
            if (byte < 0) {
                return std::nullopt;
            }
            c = static_cast<char>(byte);
        }
        output[written++] = c;
    }
    output.resize(written);
    return output;
}

// Compare a percent-encoded component to its decoded value without allocating.
// Percent mode treats '+' literally; form mode converts it to a space. Malformed
// percent escapes never compare equal.
[[nodiscard]] inline bool url_component_equals(
    std::string_view encoded, std::string_view decoded, url_decode_mode mode) noexcept {
    if (encoded.size() < decoded.size()) {
        return false;
    }

    std::size_t out = 0;
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        char c = encoded[i];
        if (mode == url_decode_mode::form && c == '+') {
            c = ' ';
        } else if (c == '%') {
            const int byte = detail::decode_percent_byte(encoded, i);
            if (byte < 0) {
                return false;
            }
            c = static_cast<char>(byte);
        }
        if (out >= decoded.size() || decoded[out] != c) {
            return false;
        }
        ++out;
    }
    return out == decoded.size();
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
            return detail::decode_percent_byte(input, i);
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

// Visit raw, borrowed name/value views for each non-empty pair. A bool-returning
// visitor returning false stops iteration and makes this return false; a void
// visitor always traverses all pairs. Pair traversal does not validate percent
// escapes. The input storage must remain alive while each view is used.
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

// Returns the raw (still percent-encoded) borrowed value of the LAST pair whose
// name matches decoded_name. Later duplicates override earlier ones. The value
// must be decoded separately before use; input storage must outlive the view.
[[nodiscard]] inline std::optional<std::string_view> find_url_encoded_value(
    std::string_view input, std::string_view decoded_name, url_decode_mode mode) {
    std::optional<std::string_view> result;
    (void)visit_url_encoded_pairs(input, [&](std::string_view name, std::string_view value) {
        if (url_component_equals(name, decoded_name, mode)) {
            result = value;
        }
        return true;
    });
    return result;
}

template <detail::http_temporary_owning_char_string input_type>
std::optional<std::string_view> find_url_encoded_value(input_type&&, std::string_view, url_decode_mode) = delete;

}  // namespace ruvia
