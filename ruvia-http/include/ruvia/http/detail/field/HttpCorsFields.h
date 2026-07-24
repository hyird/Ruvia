#pragma once

#include <string_view>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/util/HttpOws.h"
#include "ruvia/http/detail/parser/HttpRequestTarget.h"
#include "ruvia/http/detail/parser/HttpSerializedOrigin.h"

namespace ruvia::detail {

// RFC 6454 section 7.1 permits either `null` or a space-delimited list of
// serialized origins. Fetch-generated CORS requests currently send one item,
// but the HTTP protocol primitive must retain the complete field grammar.
[[nodiscard]] inline bool isValidHttpOriginFieldValue(std::string_view value) noexcept {
    value = httpTrimOws(value);
    if (value == "null") {
        return true;
    }
    std::size_t offset = 0;
    for (;;) {
        const auto separator = value.find(' ', offset);
        const auto end = separator == std::string_view::npos ? value.size() : separator;
        if (!isValidHttpSerializedOrigin(value.substr(offset, end - offset))) {
            return false;
        }
        if (separator == std::string_view::npos) {
            return true;
        }
        offset = separator + 1;
    }
}

[[nodiscard]] inline bool isValidHttpCorsRequestMethod(std::string_view value) noexcept {
    return isValidHttpMethodToken(value);
}

template <typename Visitor>
[[nodiscard]] inline bool visitHttpCorsRequestHeaderNames(std::string_view value, Visitor&& visitor) {
    bool sawName = false;
    std::size_t offset = 0;
    for (;;) {
        const auto separator = value.find(',', offset);
        const auto end = separator == std::string_view::npos ? value.size() : separator;
        auto name = value.substr(offset, end - offset);
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) {
            name.remove_prefix(1);
        }
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
            name.remove_suffix(1);
        }
        if (!name.empty()) {
            if (!isValidHttpHeaderName(name) || !visitor(name)) {
                return false;
            }
            sawName = true;
        }
        if (separator == std::string_view::npos) {
            return sawName;
        }
        offset = separator + 1;
    }
}

[[nodiscard]] inline bool isValidHttpCorsRequestHeaderNames(std::string_view value) noexcept {
    return visitHttpCorsRequestHeaderNames(value, [](std::string_view) noexcept { return true; });
}

}  // namespace ruvia::detail
