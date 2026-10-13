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

    auto* const target_resource = pmr_resource_or_default(resource);
    std::pmr::string url(target_resource);
    url.reserve(pattern.size() + 16);

    std::size_t next_value = 0;
    auto remaining = pattern;
    std::string_view segment;
    std::string_view rest;
    while (detail::split_path_segment(remaining, segment, rest)) {
        remaining = rest;

        if (segment == "*" && remaining.empty()) {
            if (next_value >= values.size()) {
                throw std::invalid_argument("url_for is missing a value for '*'");
            }
            auto value = values[next_value++];
            if (has_url_for_dot_segment(value)) {
                throw std::invalid_argument("url_for '*' value must not contain a '.' or '..' segment");
            }
            // The router refuses a wildcard capture whose decoding adds a
            // separator, so neither '\' nor a leading '/' at the root (which
            // could only be spelled "%2F" to avoid a network-path "//") would
            // ever route back to this value.
            if (value.find('\\') != std::string_view::npos || (url.empty() && value.starts_with('/'))) {
                throw std::invalid_argument("url_for '*' value has a separator the route cannot capture");
            }
            // An empty capture addresses the bare mount path itself, which is
            // exactly what the wildcard route matches for it.
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
            append_url_for_value(url, value, /*keep_slashes=*/false);
        } else {
            if (is_url_for_dot_segment(segment)) {
                throw std::invalid_argument("url_for pattern must not contain a '.' or '..' segment");
            }
            url.append(segment.data(), segment.size());
        }
    }
    if (next_value != values.size()) {
        throw std::invalid_argument("url_for received more values than the pattern has parameters");
    }
    // Only a pattern starting with an empty segment can still produce "//".
    if (url.starts_with("//")) {
        throw std::invalid_argument("url_for pattern must not start with an empty segment");
    }
    return url;
}

}  // namespace ruvia
