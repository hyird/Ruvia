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

[[nodiscard]] inline std::size_t httpMultipartBoundaryLineSize(std::string_view boundary) noexcept {
    return boundary.size() + 2;
}

[[nodiscard]] inline std::size_t httpMultipartBoundaryPrefixSize(std::string_view boundary) noexcept {
    return boundary.size() + 4;
}

[[nodiscard]] inline bool httpMultipartBoundaryAt(
    std::string_view input,
    std::size_t offset,
    std::string_view prefix,
    std::string_view boundary) noexcept {
    const auto markerSize = prefix.size() + boundary.size();
    if (offset > input.size() || input.size() - offset < markerSize) {
        return false;
    }
    if (input.substr(offset, prefix.size()) != prefix ||
        input.substr(offset + prefix.size(), boundary.size()) != boundary) {
        return false;
    }
    // A real delimiter line ends the boundary token with CRLF (next part) or "--" (close);
    // a boundary that is merely a prefix of a longer content token (e.g. "abc" inside "abcXYZ")
    // is NOT a delimiter. At end-of-buffer the match is incomplete — leave that to the caller.
    const auto after = offset + markerSize;
    if (after == input.size()) {
        return true;
    }
    const char terminator = input[after];
    if (terminator == '\r') {
        return true;  // CRLF -> next part
    }
    if (terminator == '-') {
        // The close-delimiter ends the boundary with "--"; a lone '-' followed by
        // other content (e.g. "--abc-x") is NOT a delimiter. At end-of-buffer the
        // second dash may be truncated, so (like the boundary-token EOF case above)
        // treat it as a possible match and leave completion to the caller.
        return after + 1 >= input.size() || input[after + 1] == '-';
    }
    return false;
}

[[nodiscard]] inline std::size_t httpFindMultipartBoundaryLine(
    std::string_view input,
    std::string_view boundary,
    std::size_t offset = 0) noexcept {
    for (auto cursor = input.find("--", offset);
         cursor != std::string_view::npos;
         cursor = input.find("--", cursor + 1)) {
        if (httpMultipartBoundaryAt(input, cursor, "--", boundary)) {
            return cursor;
        }
    }
    return std::string_view::npos;
}

[[nodiscard]] inline std::size_t httpFindMultipartBoundaryPrefix(
    std::string_view input,
    std::string_view boundary,
    std::size_t offset = 0) noexcept {
    for (auto cursor = input.find("\r\n--", offset);
         cursor != std::string_view::npos;
         cursor = input.find("\r\n--", cursor + 1)) {
        if (httpMultipartBoundaryAt(input, cursor, "\r\n--", boundary)) {
            return cursor;
        }
    }
    return std::string_view::npos;
}

[[nodiscard]] inline std::optional<std::string_view> httpHeaderValueInBlock(
    std::string_view headers,
    std::string_view name) noexcept {
    std::optional<std::string_view> result;
    while (!headers.empty()) {
        const auto lineEnd = headers.find("\r\n");
        const auto line = lineEnd == std::string_view::npos ? headers : headers.substr(0, lineEnd);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos) {
            const auto key = httpTrimOws(line.substr(0, colon));
            if (asciiEqualsIgnoreCase(key, name)) {
                result = httpTrimOws(line.substr(colon + 1));
            }
        }

        if (lineEnd == std::string_view::npos) {
            break;
        }
        headers.remove_prefix(lineEnd + 2);
    }

    return result;
}

[[nodiscard]] inline std::optional<std::string_view> httpDispositionParameter(
    std::string_view disposition,
    std::string_view name) noexcept {
    const auto value = httpFindSemicolonParameterQuoted(disposition, name);
    return value ? std::optional<std::string_view>(httpTrimQuotes(*value)) : std::nullopt;
}

[[nodiscard]] inline bool httpIsFormDataDisposition(std::string_view disposition) noexcept {
    const auto value = httpTrimOws(disposition);
    const auto semicolon = value.find(';');
    const auto type = httpTrimOws(semicolon == std::string_view::npos ? value : value.substr(0, semicolon));
    return asciiEqualsIgnoreCase(type, "form-data");
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
    if (!asciiEqualsIgnoreCase(mediaType, "multipart/form-data")) {
        return HttpMultipartBoundaryStatus::kInvalidContentType;
    }
    if (mediaEnd == std::string_view::npos) {
        return HttpMultipartBoundaryStatus::kInvalidBoundary;
    }

    contentType.remove_prefix(mediaEnd + 1);
    if (const auto value = httpFindSemicolonParameterQuotedIgnoreCase(contentType, "boundary")) {
        boundary = httpTrimQuotes(*value);
    }
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
