#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string_view>

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
    static constexpr char hex_digits[] = "0123456789ABCDEF";
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
        const auto raw = static_cast<unsigned char>(*escaped);
        const char encoded[] = {'%', hex_digits[raw >> 4], hex_digits[raw & 0x0F]};
        output.append(encoded, sizeof(encoded));
        value.remove_prefix(plain_size + 1);
    }
}

}  // namespace

std::pmr::string detail::route_table::url_for(std::string_view pattern,
    std::span<const std::string_view> values, std::pmr::memory_resource* resource) const {
    bool registered = false;
    for (const auto& route : routes_) {
        if (route.path() == pattern) {
            registered = true;
            break;
        }
    }
    if (!registered) {
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
            const auto value = values[next_value++];
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
            append_url_for_value(url, value, /*keep_slashes=*/false);
        } else {
            url.append(segment.data(), segment.size());
        }
    }
    if (next_value != values.size()) {
        throw std::invalid_argument("url_for received more values than the pattern has parameters");
    }
    if (pattern.ends_with('/')) {
        url.push_back('/');
    }
    return url;
}

}  // namespace ruvia
