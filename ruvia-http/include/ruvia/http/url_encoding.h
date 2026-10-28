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

// Repairs an application-supplied RFC 3986 URI-reference (for example a
// Location field value) by percent-encoding every byte that cannot appear
// literally at its position. Well-formed pct-encoded triplets and every legal
// byte are kept byte for byte, so a correctly encoded reference is never
// encoded twice (RFC 3986 section 2.4). '[' and ']' stay literal only as the
// authority's IP-literal delimiters, ':' is encoded in the first segment of a
// relative-path reference (section 4.2), '@' inside userinfo is encoded so only
// the delimiting '@' remains (section 3.2.1), and only the first '#' starts the
// fragment. Returns no value, without allocating, when the reference needs no
// encoding; otherwise the encoded reference is allocated from `resource` (or
// the default PMR resource).
[[nodiscard]] std::optional<std::pmr::string> percent_encode_uri_reference(
    std::string_view reference, std::pmr::memory_resource* resource);

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

// Returns the complete decoded component. Percent mode treats '+' literally and
// returns no value for malformed percent encoding (RFC 3986 pct-encoded). Form
// mode converts '+' to a space and, like the WHATWG percent-decode algorithm
// used by application/x-www-form-urlencoded, keeps a '%' that does not start a
// valid escape as a literal byte.
// The result is allocated from options.resource (or the default PMR resource),
// which must outlive the returned string. On failure, no decoded prefix escapes.
[[nodiscard]] inline std::optional<std::pmr::string> decode_url_component(
    std::string_view input, url_decode_options options = {}) {
    auto* const resource = detail::http_pmr_resource_or_default(options.resource_);
    std::size_t decoded_size = 0;
    bool transformed = false;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (options.mode_ == url_decode_mode::form && c == '+') {
            transformed = true;
        } else if (c == '%') {
            if (http_try_decode_percent_byte(input, i).has_value()) {
                transformed = true;
                i += 2;
            } else if (options.mode_ != url_decode_mode::form) {
                return std::nullopt;
            }
        }
        ++decoded_size;
    }
    if (!transformed) {
        return std::pmr::string(input, resource);
    }
    std::pmr::string output(resource);
    output.reserve(decoded_size);
    for (std::size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (options.mode_ == url_decode_mode::form && c == '+') {
            c = ' ';
        } else if (c == '%') {
            if (const auto byte = http_try_decode_percent_byte(input, i)) {
                c = static_cast<char>(*byte);
                i += 2;
            }
        }
        output.push_back(c);
    }
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

// Origin-form path identity, the one canonical form shared by route matching,
// path-scoped middleware and fallbacks, and decoded route parameters. A path
// splits into segments on literal '/' only, and each segment is its
// percent-decoded octets. Hex case and encoded unreserved characters therefore
// never make two spellings differ (RFC 3986 6.2.2.1, 6.2.2.2), and an encoded
// "%2F" stays inside its segment instead of becoming a boundary. The form is
// deliberately coarser than RFC 3986 equivalence: an encoded reserved
// character equals its literal ("%3A" and ":"), because a parameter decoded
// from either spelling reads the same value. RFC 9110 4.2.3 only forbids
// splitting equivalent URIs across resources, not merging non-equivalent ones.
//
// The canonical unit at path[index] (index < path.size()) is its literal or
// decoded octet (0-255), uri_path_separator_unit for a literal '/', or -1 for
// a malformed escape; next_uri_path_unit returns it and advances index past it.
inline constexpr int uri_path_separator_unit = 0x100;

[[nodiscard]] constexpr int next_uri_path_unit(std::string_view path, std::size_t& index) noexcept {
    const char c = path[index];
    if (c == '/') {
        ++index;
        return uri_path_separator_unit;
    }
    if (c == '%') {
        const auto byte = http_try_decode_percent_byte(path, index);
        if (!byte) {
            ++index;
            return -1;
        }
        index += 3;
        return *byte;
    }
    ++index;
    return static_cast<unsigned char>(c);
}

// Three-way order of two paths by their canonical units: negative, zero or
// positive like std::string_view::compare. A total order over well-formed
// paths, consistent with uri_paths_equivalent, for sorted lookups.
[[nodiscard]] constexpr int compare_uri_paths(std::string_view left, std::string_view right) noexcept {
    std::size_t left_index = 0;
    std::size_t right_index = 0;
    while (left_index < left.size() && right_index < right.size()) {
        const auto left_unit = next_uri_path_unit(left, left_index);
        const auto right_unit = next_uri_path_unit(right, right_index);
        if (left_unit != right_unit) {
            return left_unit < right_unit ? -1 : 1;
        }
    }
    if (left_index < left.size()) {
        return 1;
    }
    return right_index < right.size() ? -1 : 0;
}

// Whether two paths (or two segments) have the same canonical form. Malformed
// escapes never compare equal. Spellings without '%' compare byte for byte.
[[nodiscard]] constexpr bool uri_paths_equivalent(std::string_view left, std::string_view right) noexcept {
    std::size_t left_index = 0;
    std::size_t right_index = 0;
    while (left_index < left.size() && right_index < right.size()) {
        const auto left_unit = next_uri_path_unit(left, left_index);
        if (left_unit < 0 || left_unit != next_uri_path_unit(right, right_index)) {
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
