#pragma once

#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "HeaderTokenUtils.h"

namespace ruvia::detail {

inline void httpAssignMultipartBoundaryMarkers(
    std::pmr::string& line,
    std::pmr::string& prefix,
    std::string_view boundary) {
    line.clear();
    prefix.clear();

    line.reserve(boundary.size() + 2);
    line.append("--");
    line.append(boundary.data(), boundary.size());

    prefix.reserve(line.size() + 2);
    prefix.append("\r\n");
    prefix.append(line.data(), line.size());
}

[[nodiscard]] inline std::optional<std::string_view> httpHeaderValueInBlock(
    std::string_view headers,
    std::string_view name) noexcept {
    while (!headers.empty()) {
        const auto lineEnd = headers.find("\r\n");
        const auto line = lineEnd == std::string_view::npos ? headers : headers.substr(0, lineEnd);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos) {
            const auto key = httpTrimOws(line.substr(0, colon));
            if (httpAsciiEqualsIgnoreCase(key, name)) {
                return httpTrimOws(line.substr(colon + 1));
            }
        }

        if (lineEnd == std::string_view::npos) {
            break;
        }
        headers.remove_prefix(lineEnd + 2);
    }

    return std::nullopt;
}

[[nodiscard]] inline std::optional<std::string_view> httpDispositionParameter(
    std::string_view disposition,
    std::string_view name) noexcept {
    std::optional<std::string_view> result;
    httpVisitSemicolonParameters(disposition, [name, &result](std::string_view key, std::string_view value) noexcept {
        if (key == name) {
            result = httpTrimQuotes(value);
            return false;
        }
        return true;
    });
    return result;
}

[[nodiscard]] inline bool httpIsFormDataDisposition(std::string_view disposition) noexcept {
    const auto value = httpTrimOws(disposition);
    const auto semicolon = value.find(';');
    const auto type = httpTrimOws(semicolon == std::string_view::npos ? value : value.substr(0, semicolon));
    return httpAsciiEqualsIgnoreCase(type, "form-data");
}

struct HttpMultipartPartHeaders final {
    std::string_view name;
    std::string_view filename;
    std::string_view contentType;
};

enum class HttpMultipartPartHeaderStatus {
    kOk,
    kInvalidDisposition,
    kMissingName
};

enum class HttpMultipartBoundaryStatus {
    kOk,
    kInvalidContentType,
    kInvalidBoundary
};

[[nodiscard]] inline HttpMultipartBoundaryStatus httpParseMultipartBoundary(
    std::string_view contentType,
    std::string_view& boundary) noexcept {
    boundary = {};
    const auto mediaEnd = contentType.find(';');
    const auto mediaType = httpTrimOws(
        mediaEnd == std::string_view::npos ? contentType : contentType.substr(0, mediaEnd));
    if (!httpAsciiEqualsIgnoreCase(mediaType, "multipart/form-data")) {
        return HttpMultipartBoundaryStatus::kInvalidContentType;
    }
    if (mediaEnd == std::string_view::npos) {
        return HttpMultipartBoundaryStatus::kInvalidBoundary;
    }

    contentType.remove_prefix(mediaEnd + 1);
    httpVisitSemicolonParameters(contentType, [&boundary](std::string_view key, std::string_view value) noexcept {
        if (httpAsciiEqualsIgnoreCase(key, "boundary")) {
            boundary = httpTrimQuotes(value);
            return false;
        }
        return true;
    });
    return boundary.empty()
        ? HttpMultipartBoundaryStatus::kInvalidBoundary
        : HttpMultipartBoundaryStatus::kOk;
}

[[nodiscard]] inline HttpMultipartPartHeaderStatus httpParseMultipartPartHeaders(
    std::string_view headers,
    HttpMultipartPartHeaders& output) noexcept {
    output = {};
    const auto disposition = httpHeaderValueInBlock(headers, "Content-Disposition");
    if (!disposition || !httpIsFormDataDisposition(*disposition)) {
        return HttpMultipartPartHeaderStatus::kInvalidDisposition;
    }

    const auto name = httpDispositionParameter(*disposition, "name");
    if (!name) {
        return HttpMultipartPartHeaderStatus::kMissingName;
    }

    output.name = *name;
    if (const auto filename = httpDispositionParameter(*disposition, "filename")) {
        output.filename = *filename;
    }
    if (const auto contentType = httpHeaderValueInBlock(headers, "Content-Type")) {
        output.contentType = *contentType;
    }
    return HttpMultipartPartHeaderStatus::kOk;
}

}  // namespace ruvia::detail
