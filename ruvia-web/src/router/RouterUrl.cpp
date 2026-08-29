#include "ruvia/web/detail/router/RouteTable.h"

#include <stdexcept>
#include <string_view>

// Building a URL from a registered route pattern: substitute the parameters in
// order and percent-encode whatever a path segment may not carry verbatim. This
// is the reverse of resolution and shares nothing with it.

namespace ruvia {

namespace {

// RFC 3986 pchar minus pct-encoded: bytes a path segment may carry verbatim.
[[nodiscard]] constexpr bool isUrlForSegmentByte(char value) noexcept {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') || value == '-' || value == '.' || value == '_' ||
           value == '~' || value == '!' || value == '$' || value == '&' || value == '\'' ||
           value == '(' || value == ')' || value == '*' || value == '+' || value == ',' ||
           value == ';' || value == '=' || value == ':' || value == '@';
}

void appendUrlForValue(std::pmr::string& output, std::string_view value, bool keepSlashes) {
    static constexpr char kHexDigits[] = "0123456789ABCDEF";
    for (const char byte : value) {
        if (isUrlForSegmentByte(byte) || (keepSlashes && byte == '/')) {
            output.push_back(byte);
            continue;
        }
        const auto raw = static_cast<unsigned char>(byte);
        output.push_back('%');
        output.push_back(kHexDigits[raw >> 4]);
        output.push_back(kHexDigits[raw & 0x0F]);
    }
}

}  // namespace

std::pmr::string detail::RouteTable::urlFor(std::string_view pattern,
    std::span<const std::string_view> values, std::pmr::memory_resource* resource) const {
    bool registered = false;
    for (const auto& route : routes_) {
        if (route.path() == pattern) {
            registered = true;
            break;
        }
    }
    if (!registered) {
        throw std::invalid_argument("urlFor pattern is not a registered route");
    }

    auto* const targetResource = pmrResourceOrDefault(resource);
    std::pmr::string url(targetResource);
    url.reserve(pattern.size() + 16);

    std::size_t nextValue = 0;
    auto remaining = pattern;
    if (remaining.starts_with('/')) {
        remaining.remove_prefix(1);
    }
    if (remaining.empty()) {
        url.push_back('/');
    }
    while (!remaining.empty()) {
        const auto slash = remaining.find('/');
        const auto segment =
            slash == std::string_view::npos ? remaining : remaining.substr(0, slash);
        remaining =
            slash == std::string_view::npos ? std::string_view{} : remaining.substr(slash + 1);

        if (segment == "*" && remaining.empty()) {
            if (nextValue >= values.size()) {
                throw std::invalid_argument("urlFor is missing a value for '*'");
            }
            const auto value = values[nextValue++];
            // An empty capture addresses the bare mount path itself, which is
            // exactly what the wildcard route matches for it.
            if (!value.empty()) {
                url.push_back('/');
                appendUrlForValue(url, value, /*keepSlashes=*/true);
            } else if (url.empty()) {
                url.push_back('/');
            }
            continue;
        }
        url.push_back('/');
        if (!segment.empty() && segment.front() == ':') {
            if (nextValue >= values.size()) {
                throw std::invalid_argument("urlFor is missing a route parameter value");
            }
            const auto value = values[nextValue++];
            if (value.empty()) {
                throw std::invalid_argument("urlFor route parameter value must not be empty");
            }
            appendUrlForValue(url, value, /*keepSlashes=*/false);
        } else {
            url.append(segment.data(), segment.size());
        }
    }
    if (nextValue != values.size()) {
        throw std::invalid_argument("urlFor received more values than the pattern has parameters");
    }
    return url;
}

}  // namespace ruvia
