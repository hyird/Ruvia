#pragma once

#include "ruvia/http/Error.h"
#include "ruvia/http/HeaderUtils.h"

#include <cstddef>
#include <ctime>
#include <memory_resource>
#include <string>
#include <string_view>

namespace ruvia::detail {

struct FileConditionalHeaders final {
    std::string_view ifMatch;
    std::string_view ifUnmodifiedSince;
    std::string_view ifNoneMatch;
    std::string_view ifModifiedSince;
    std::string_view range;
    std::string_view ifRange;
};

[[nodiscard]] inline bool etagListMatches(
    std::string_view values,
    std::string_view expected,
    bool strong) noexcept {
    while (!values.empty()) {
        const auto comma = values.find(',');
        const auto value = httpTrimOws(
            comma == std::string_view::npos ? values : values.substr(0, comma));
        if (strong ? httpStrongEtagEquals(value, expected) : httpWeakEtagEquals(value, expected)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        values.remove_prefix(comma + 1);
    }
    return false;
}

[[nodiscard]] inline bool ifMatchAllows(std::string_view header, std::string_view etag) noexcept {
    if (header.empty()) {
        return true;
    }
    if (httpTrimOws(header) == "*") {
        return true;
    }
    return etagListMatches(header, etag, true);
}

[[nodiscard]] inline bool ifNoneMatchMatches(std::string_view header, std::string_view etag) noexcept {
    if (header.empty()) {
        return false;
    }
    if (httpTrimOws(header) == "*") {
        return true;
    }
    return etagListMatches(header, etag, false);
}

[[nodiscard]] inline bool httpDateNotModified(std::string_view header, std::time_t modifiedSeconds) noexcept {
    const auto date = httpParseImfFixdate(httpTrimOws(header));
    return date.has_value() && modifiedSeconds <= *date;
}

[[nodiscard]] inline bool httpDateUnmodified(std::string_view header, std::time_t modifiedSeconds) noexcept {
    const auto date = httpParseImfFixdate(httpTrimOws(header));
    return !date.has_value() || modifiedSeconds <= *date;
}

[[nodiscard]] inline bool ifRangeAllows(
    std::string_view header,
    std::string_view etag,
    std::time_t modifiedSeconds) noexcept {
    if (header.empty()) {
        return true;
    }
    const auto value = httpTrimOws(header);
    if (!value.empty() && (value.front() == '"' || value.starts_with("W/"))) {
        return httpStrongEtagEquals(value, etag);
    }
    return httpDateNotModified(value, modifiedSeconds);
}

[[nodiscard]] inline bool isWindowsDrivePath(std::string_view path) noexcept {
    if (path.size() < 2 || path[1] != ':') {
        return false;
    }
    const auto c = path.front();
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

[[nodiscard]] inline std::pmr::string normalizeStaticRelativePath(
    std::string_view input,
    std::pmr::polymorphic_allocator<char> allocator) {
    if (!input.empty() && (input.front() == '/' || input.front() == '\\' || isWindowsDrivePath(input))) {
        throw HttpError(403, "forbidden", "invalid static file path");
    }

    std::pmr::string output(allocator);
    output.reserve(input.size());
    std::size_t cursor = 0;
    while (cursor <= input.size()) {
        const auto slash = input.find_first_of("/\\", cursor);
        const auto end = slash == std::string_view::npos ? input.size() : slash;
        const auto segment = input.substr(cursor, end - cursor);

        if (!segment.empty() && segment != ".") {
            if (segment == "..") {
                if (output.empty()) {
                    throw HttpError(403, "forbidden", "invalid static file path");
                }
                const auto previousSlash = output.rfind('/');
                if (previousSlash == std::pmr::string::npos) {
                    output.clear();
                } else {
                    output.erase(previousSlash);
                }
            } else {
                if (!output.empty()) {
                    output.push_back('/');
                }
                output.append(segment.data(), segment.size());
            }
        }

        if (slash == std::string_view::npos) {
            break;
        }
        cursor = slash + 1;
    }

    return output;
}

}  // namespace ruvia::detail
