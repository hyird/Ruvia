#pragma once

#include <optional>
#include <string_view>

#include "ruvia/http/detail/header/HeaderTokenUtils.h"

namespace ruvia::detail {

[[nodiscard]] inline std::optional<std::string_view> httpContentTypeParameter(
    std::string_view contentType,
    std::string_view name) noexcept {
    while (!contentType.empty()) {
        const auto semicolon = contentType.find(';');
        const auto part = httpTrimOws(
            semicolon == std::string_view::npos ? contentType : contentType.substr(0, semicolon));
        const auto equals = part.find('=');
        if (equals != std::string_view::npos) {
            const auto key = httpTrimOws(part.substr(0, equals));
            if (httpAsciiEqualsIgnoreCase(key, name)) {
                return httpTrimQuotes(httpTrimOws(part.substr(equals + 1)));
            }
        }
        if (semicolon == std::string_view::npos) {
            break;
        }
        contentType.remove_prefix(semicolon + 1);
    }
    return std::nullopt;
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
    while (!disposition.empty()) {
        const auto semicolon = disposition.find(';');
        const auto part = httpTrimOws(
            semicolon == std::string_view::npos ? disposition : disposition.substr(0, semicolon));
        const auto equals = part.find('=');
        if (equals != std::string_view::npos) {
            const auto key = httpTrimOws(part.substr(0, equals));
            if (key == name) {
                return httpTrimQuotes(httpTrimOws(part.substr(equals + 1)));
            }
        }

        if (semicolon == std::string_view::npos) {
            break;
        }
        disposition.remove_prefix(semicolon + 1);
    }

    return std::nullopt;
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
    while (!contentType.empty()) {
        const auto semicolon = contentType.find(';');
        const auto part = httpTrimOws(
            semicolon == std::string_view::npos ? contentType : contentType.substr(0, semicolon));
        const auto equals = part.find('=');
        if (equals != std::string_view::npos) {
            const auto key = httpTrimOws(part.substr(0, equals));
            if (httpAsciiEqualsIgnoreCase(key, "boundary")) {
                boundary = httpTrimQuotes(httpTrimOws(part.substr(equals + 1)));
                return boundary.empty()
                    ? HttpMultipartBoundaryStatus::kInvalidBoundary
                    : HttpMultipartBoundaryStatus::kOk;
            }
        }
        if (semicolon == std::string_view::npos) {
            break;
        }
        contentType.remove_prefix(semicolon + 1);
    }
    return HttpMultipartBoundaryStatus::kInvalidBoundary;
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
