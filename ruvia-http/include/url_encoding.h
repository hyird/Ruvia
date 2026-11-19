#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "binary_text_encoding.h"
#include "borrowed_text.h"

namespace ruvia {

// percent: RFC 3986 pct-encoded decoding; '+' is literal and a malformed
// escape fails.
// path: percent decoding that also fails on an escape revealing '/' or '\\',
// so decoded path text never gains a segment boundary that routing and
// path-scoped middleware, which split on literal '/' only, never saw.
// form: application/x-www-form-urlencoded; '+' is a space and, like the WHATWG
// percent-decode algorithm, a '%' that starts no valid escape stays literal.
// strict_form: the same '+' conversion, but malformed escapes fail.
enum class url_decode_mode : std::uint8_t { percent,
    path,
    form,
    strict_form };

// Percent and path modes treat '+' literally; both form modes decode it.
[[nodiscard]] inline bool has_url_encoding(std::string_view value, url_decode_mode mode) noexcept {
    // Keep short components and early escapes on the scalar path; library searches
    // can scan the remaining literal text in larger chunks.
    constexpr std::size_t scalar_scan_limit = 64;
    constexpr std::size_t scalar_prefix_size = 32;
    const auto prefix = value.substr(0, value.size() <= scalar_scan_limit ? value.size() : scalar_prefix_size);
    const bool encoded = (mode == url_decode_mode::form || mode == url_decode_mode::strict_form)
        ? std::ranges::any_of(prefix, [](char c) noexcept {
        return c == '%' || c == '+';
    })
        : std::ranges::any_of(prefix, [](char c) noexcept {
        return c == '%';
    });
    if (encoded) {
        return true;
    }
    if (prefix.size() == value.size()) {
        return false;
    }
    value.remove_prefix(prefix.size());
    return ((mode == url_decode_mode::form || mode == url_decode_mode::strict_form) ? value.find_first_of("%+") : value.find('%')) != std::string_view::npos;
}

// Decodes the pct-encoded triplet starting at input[index] (RFC 3986 2.1):
// '%' followed by two hex digits. Returns no value when the triplet is
// truncated, does not start with '%', or contains a non-hex digit.
[[nodiscard]] constexpr std::optional<unsigned char> decode_percent_byte(std::string_view input, std::size_t index) noexcept {
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

// RFC 3986 section 2.3 unreserved: ALPHA / DIGIT / "-" / "." / "_" / "~".
// Percent-encoding one of these octets is equivalent to spelling it literally
// (section 6.2.2.2), so encoders keep them literal.
[[nodiscard]] constexpr bool is_uri_unreserved_byte(unsigned char byte) noexcept {
    return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || byte == '-' || byte == '.' || byte == '_' || byte == '~';
}

// Appends the canonical uppercase pct-encoded triplet for byte. Callers own the
// policy deciding which bytes need escaping. Failure leaves output unchanged.
inline void append_percent_byte(std::string& output, unsigned char byte) {
    const char triplet[]{'%', http_upper_hex_digit(byte >> 4), http_upper_hex_digit(byte)};
    output.append(triplet, sizeof(triplet));
}

// Encodes one component: only RFC 3986 unreserved octets remain literal;
// every other octet (including '%' and '+') becomes uppercase %XX.
// Borrows unchanged input, otherwise replaces storage. Input may alias storage.
// Allocation/length failure preserves storage. Owning temporaries are rejected.
[[nodiscard]] std::string_view percent_encode_url_component(borrowed_text text, std::string& storage);

// Repairs an application-supplied RFC 3986 URI-reference (for example a
// Location field value) by percent-encoding every byte that cannot appear
// literally at its position. Well-formed pct-encoded triplets and every legal
// byte are kept byte for byte, so a correctly encoded reference is never
// encoded twice (RFC 3986 section 2.4). '[' and ']' stay literal only as the
// authority's IP-literal delimiters, ':' is encoded in the first segment of a
// relative-path reference (section 4.2), '@' inside userinfo is encoded so only
// the delimiting '@' remains (section 3.2.1), and only the first '#' starts the
// fragment. Returns `reference` itself, without allocating, when it needs no
// encoding; otherwise the encoded reference replaces `storage`'s contents and
// the result views `storage`. Failure preserves storage's original contents.
// `reference` may alias `storage`. Keep the input owner alive until the result
// is consumed; owning-string temporaries are rejected even when their particular
// contents would require transformation.
[[nodiscard]] std::string_view percent_encode_uri_reference(borrowed_text reference, std::string& storage);

namespace detail {
template <typename visitor_type>
[[nodiscard]] bool dispatch_url_encoded_pair_visitor(visitor_type& visitor, std::string_view name, std::string_view value) {
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

[[nodiscard]] inline bool is_valid_url_encoding(std::string_view value) noexcept {
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '%') {
            continue;
        }
        if (!decode_percent_byte(value, i)) {
            return false;
        }
        i += 2;
    }
    return true;
}

// The single URL decode loop: decodes `input` under `mode` in one pass into
// `output`, which holds at least input.size() bytes and does not overlap
// `input`, and returns the decoded size, or no value when `mode` rejects the
// input. Callers that pack several decoded values into one buffer use this
// directly.
[[nodiscard]] constexpr std::optional<std::size_t> decode_url_bytes(std::string_view input, char* output, url_decode_mode mode) noexcept {
    std::size_t size = 0;
    for (std::size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if ((mode == url_decode_mode::form || mode == url_decode_mode::strict_form) && c == '+') {
            c = ' ';
        } else if (c == '%') {
            if (const auto byte = decode_percent_byte(input, i)) {
                if (mode == url_decode_mode::path && (*byte == '/' || *byte == '\\')) {
                    return std::nullopt;
                }
                c = static_cast<char>(*byte);
                i += 2;
            } else if (mode != url_decode_mode::form) {
                return std::nullopt;
            }
        }
        output[size++] = c;
    }
    return size;
}

// Decodes one URL component under `mode`. Returns `input` itself, without
// allocating, when it holds nothing to decode; otherwise the decoded bytes
// replace `storage`'s contents and the result views `storage`. Input `mode`
// rejects returns no value and leaves `storage` unchanged. `input` may alias
// `storage`. Keep its owner alive until the result is consumed; owning-string
// temporaries are rejected regardless of their contents.
[[nodiscard]] std::optional<std::string_view> decode_url_component(borrowed_text text, std::string& storage, url_decode_mode mode = url_decode_mode::percent);

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
        const auto byte = decode_percent_byte(path, index);
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

enum class url_empty_pair_policy : std::uint8_t { skip,
    preserve };

// Visits raw, borrowed name/value views in order, including repeated names.
// Empty segments are skipped by default. preserve emits ("", "") for every
// empty segment, including the trailing one and one for empty input.
// A false visitor result stops traversal and returns false; void visits all.
// Percent escapes are not validated. Input must outlive the borrowed views.
template <typename visitor_type>
[[nodiscard]] bool visit_url_encoded_pairs(std::string_view input, visitor_type&& visitor, url_empty_pair_policy empty_pairs = url_empty_pair_policy::skip) {
    auto& visitor_ref = visitor;
    for (;;) {
        // Find the first separator, then scan only the remaining value.
        const auto separator = input.find_first_of("=&");
        const bool has_value = separator != std::string_view::npos && input[separator] == '=';
        const auto pair_end = has_value ? input.find('&', separator + 1) : separator;
        const auto pair = input.substr(0, pair_end);

        if (!pair.empty() || empty_pairs == url_empty_pair_policy::preserve) {
            const auto name = has_value ? pair.substr(0, separator) : pair;
            const auto value = has_value ? pair.substr(separator + 1) : std::string_view{};
            if (!detail::dispatch_url_encoded_pair_visitor(visitor_ref, name, value)) {
                return false;
            }
        }

        if (pair_end == std::string_view::npos) {
            return true;
        }
        input.remove_prefix(pair_end + 1);
    }
}

}  // namespace ruvia
