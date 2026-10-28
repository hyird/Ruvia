#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/url_encoding.h"

#include "router/path_segments.h"
#include "router/route_table.h"

// Substitute parameters and percent-encode path values while preserving the
// same segments used to build and resolve the registered route.

namespace ruvia {

namespace {

// RFC 3986 pchar minus pct-encoded: bytes a path segment may carry verbatim.
[[nodiscard]] constexpr bool is_url_for_segment_byte(char value) noexcept {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') || value == '-' || value == '.' || value == '_' ||
           value == '~' || value == '!' || value == '$' || value == '&' || value == '\'' ||
           value == '(' || value == ')' || value == '*' || value == '+' || value == ',' ||
           value == ';' || value == '=' || value == ':' || value == '@';
}

[[nodiscard]] constexpr std::size_t url_for_encoded_size(
    std::string_view value, bool keep_slashes) noexcept {
    std::size_t size = value.size();
    for (const char byte : value) {
        if (!is_url_for_segment_byte(byte) && !(keep_slashes && byte == '/')) {
            size += 2U;
        }
    }
    return size;
}

void append_url_for_value(std::pmr::string& output, std::string_view value, bool keep_slashes) {
    while (!value.empty()) {
        const auto escaped = std::ranges::find_if_not(value, [keep_slashes](char byte) noexcept {
            return is_url_for_segment_byte(byte) || (keep_slashes && byte == '/');
        });
        const auto plain_size = static_cast<std::size_t>(escaped - value.begin());
        if (plain_size != 0) {
            output.append(value.data(), plain_size);
        }
        if (escaped == value.end()) {
            return;
        }
        http_append_percent_byte(output, static_cast<unsigned char>(*escaped));
        value.remove_prefix(plain_size + 1);
    }
}

// RFC 3986 5.2.4 resolvers remove "." and ".." segments, and WHATWG URL
// parsers treat "%2E" spellings the same way, so encoding cannot keep them.
[[nodiscard]] constexpr bool is_url_for_dot_segment(std::string_view segment) noexcept {
    return segment == "." || segment == "..";
}

[[nodiscard]] bool has_url_for_dot_segment(std::string_view value) noexcept {
    while (true) {
        const auto slash = value.find('/');
        if (is_url_for_dot_segment(value.substr(0, slash))) {
            return true;
        }
        if (slash == std::string_view::npos) {
            return false;
        }
        value.remove_prefix(slash + 1);
    }
}

}  // namespace

std::pmr::string detail::route_table::url_for(std::string_view pattern,
    std::span<const std::string_view> values, std::pmr::memory_resource* resource) const {
    if (!std::ranges::binary_search(plan_->url_patterns_, pattern)) {
        throw std::invalid_argument("url_for pattern is not a registered route");
    }

    // Validate every segment and value before allocating, and size the buffer
    // from the actual parameter values so short URLs stay in SSO and rejected
    // calls never touch a monotonic arena.
    std::size_t next_value = 0;
    std::size_t estimated_size = 0;
    auto remaining = pattern;
    std::string_view segment;
    std::string_view rest;
    while (detail::split_path_segment(remaining, segment, rest)) {
        remaining = rest;
        if (segment == "*" && remaining.empty()) {
            if (next_value >= values.size()) {
                throw std::invalid_argument("url_for is missing a value for '*'");
            }
            const auto value = values[next_value++];
            if (has_url_for_dot_segment(value)) {
                throw std::invalid_argument("url_for '*' value must not contain a '.' or '..' segment");
            }
            if (value.find('\\') != std::string_view::npos || (estimated_size == 0 && value.starts_with('/'))) {
                throw std::invalid_argument("url_for '*' value has a separator the route cannot capture");
            }
            if (!value.empty()) {
                estimated_size += 1U + url_for_encoded_size(value, /*keep_slashes=*/true);
            } else if (estimated_size == 0) {
                estimated_size += 1U;
            }
            continue;
        }
        if (estimated_size == 0 && segment.empty()) {
            throw std::invalid_argument("url_for pattern must not start with an empty segment");
        }
        estimated_size += 1U;
        if (!segment.empty() && segment.front() == ':') {
            if (next_value >= values.size()) {
                throw std::invalid_argument("url_for is missing a route parameter value");
            }
            const auto value = values[next_value++];
            if (value.empty()) {
                throw std::invalid_argument("url_for route parameter value must not be empty");
            }
            if (is_url_for_dot_segment(value)) {
                throw std::invalid_argument("url_for route parameter value must not be '.' or '..'");
            }
            estimated_size += url_for_encoded_size(value, /*keep_slashes=*/false);
        } else {
            if (is_url_for_dot_segment(segment)) {
                throw std::invalid_argument("url_for pattern must not contain a '.' or '..' segment");
            }
            estimated_size += segment.size();
        }
    }
    if (next_value != values.size()) {
        throw std::invalid_argument("url_for received more values than the pattern has parameters");
    }

    auto* const target_resource = pmr_resource_or_default(resource);
    std::pmr::string url(target_resource);
    if (estimated_size > 15U) {
        url.reserve(estimated_size);
    }

    next_value = 0;
    remaining = pattern;
    while (detail::split_path_segment(remaining, segment, rest)) {
        remaining = rest;
        if (segment == "*" && remaining.empty()) {
            const auto value = values[next_value++];
            if (!value.empty()) {
                url.push_back('/');
                append_url_for_value(url, value, /*keep_slashes=*/true);
            } else if (url.empty()) {
                url.push_back('/');
            }
            continue;
        }
        url.push_back('/');
        if (!segment.empty() && segment.front() == ':') {
            append_url_for_value(url, values[next_value++], /*keep_slashes=*/false);
        } else {
            url.append(segment.data(), segment.size());
        }
    }
    return url;
}

}  // namespace ruvia
