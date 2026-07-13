#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/MultipartParser.h"
#include "ruvia/http/detail/HeaderTokenUtils.h"

namespace ruvia::detail {

class HttpMultipartDelimiterResult;

[[nodiscard]] inline HttpMultipartDelimiterResult httpMatchMultipartDelimiterLine(
    std::string_view input,
    const MultipartBoundary& boundary,
    bool inputFinished) noexcept;
[[nodiscard]] inline HttpMultipartDelimiterResult httpFindInitialMultipartDelimiter(
    std::string_view input,
    const MultipartBoundary& boundary,
    bool inputFinished) noexcept;
[[nodiscard]] inline HttpMultipartDelimiterResult httpFindMultipartBodyDelimiter(
    std::string_view input,
    const MultipartBoundary& boundary,
    bool inputFinished) noexcept;

class HttpMultipartDelimiterNoMatch final {
private:
    friend class HttpMultipartDelimiterResult;
    constexpr HttpMultipartDelimiterNoMatch() noexcept = default;
};

class HttpMultipartDelimiterNeedInput final {
public:
    // Initial search: offset of the leading "--". Body search: offset of the
    // CRLF that belongs to the delimiter rather than the preceding part.
    [[nodiscard]] constexpr std::size_t offset() const noexcept {
        return offset_;
    }

private:
    friend class HttpMultipartDelimiterResult;

    explicit constexpr HttpMultipartDelimiterNeedInput(
        std::size_t offset) noexcept
        : offset_(offset) {}

    std::size_t offset_;
};

class HttpMultipartPartDelimiter final {
public:
    [[nodiscard]] constexpr std::size_t offset() const noexcept {
        return offset_;
    }

    [[nodiscard]] constexpr std::size_t lineBytes() const noexcept {
        return lineBytes_;
    }

private:
    friend class HttpMultipartDelimiterResult;

    constexpr HttpMultipartPartDelimiter(
        std::size_t offset,
        std::size_t lineBytes) noexcept
        : offset_(offset), lineBytes_(lineBytes) {}

    std::size_t offset_;
    std::size_t lineBytes_;
};

class HttpMultipartCloseDelimiter final {
public:
    [[nodiscard]] constexpr std::size_t offset() const noexcept {
        return offset_;
    }

    [[nodiscard]] constexpr std::size_t lineBytes() const noexcept {
        return lineBytes_;
    }

private:
    friend class HttpMultipartDelimiterResult;

    constexpr HttpMultipartCloseDelimiter(
        std::size_t offset,
        std::size_t lineBytes) noexcept
        : offset_(offset), lineBytes_(lineBytes) {}

    std::size_t offset_;
    std::size_t lineBytes_;
};

// Delimiter scanning distinguishes absence, an input-boundary ambiguity, a
// regular part delimiter, and the terminal close delimiter. Only outcomes
// that found a candidate expose its offset; only complete delimiter lines
// expose their line length.
class HttpMultipartDelimiterResult final {
public:
    [[nodiscard]] constexpr const HttpMultipartDelimiterNoMatch*
    noMatch() const noexcept {
        return std::get_if<HttpMultipartDelimiterNoMatch>(&value_);
    }

    [[nodiscard]] constexpr const HttpMultipartDelimiterNeedInput*
    needInput() const noexcept {
        return std::get_if<HttpMultipartDelimiterNeedInput>(&value_);
    }

    [[nodiscard]] constexpr const HttpMultipartPartDelimiter*
    part() const noexcept {
        return std::get_if<HttpMultipartPartDelimiter>(&value_);
    }

    [[nodiscard]] constexpr const HttpMultipartCloseDelimiter*
    close() const noexcept {
        return std::get_if<HttpMultipartCloseDelimiter>(&value_);
    }

private:
    friend HttpMultipartDelimiterResult httpMatchMultipartDelimiterLine(
        std::string_view,
        const MultipartBoundary&,
        bool) noexcept;
    friend HttpMultipartDelimiterResult httpFindInitialMultipartDelimiter(
        std::string_view,
        const MultipartBoundary&,
        bool) noexcept;
    friend HttpMultipartDelimiterResult httpFindMultipartBodyDelimiter(
        std::string_view,
        const MultipartBoundary&,
        bool) noexcept;

    using Value = std::variant<
        HttpMultipartDelimiterNoMatch,
        HttpMultipartDelimiterNeedInput,
        HttpMultipartPartDelimiter,
        HttpMultipartCloseDelimiter>;

    template <typename Result>
    explicit constexpr HttpMultipartDelimiterResult(Result result) noexcept
        : value_(result) {}

    [[nodiscard]] static constexpr HttpMultipartDelimiterResult
    makeNoMatch() noexcept {
        return HttpMultipartDelimiterResult(HttpMultipartDelimiterNoMatch());
    }

    [[nodiscard]] static constexpr HttpMultipartDelimiterResult makeNeedInput(
        std::size_t offset = 0) noexcept {
        return HttpMultipartDelimiterResult(
            HttpMultipartDelimiterNeedInput(offset));
    }

    [[nodiscard]] static constexpr HttpMultipartDelimiterResult makePart(
        std::size_t offset,
        std::size_t lineBytes) noexcept {
        return HttpMultipartDelimiterResult(
            HttpMultipartPartDelimiter(offset, lineBytes));
    }

    [[nodiscard]] static constexpr HttpMultipartDelimiterResult makeClose(
        std::size_t offset,
        std::size_t lineBytes) noexcept {
        return HttpMultipartDelimiterResult(
            HttpMultipartCloseDelimiter(offset, lineBytes));
    }

    [[nodiscard]] constexpr HttpMultipartDelimiterResult rebased(
        std::size_t base) const noexcept {
        if (const auto* needInput = this->needInput()) {
            return makeNeedInput(base + needInput->offset());
        }
        if (const auto* part = this->part()) {
            return makePart(base + part->offset(), part->lineBytes());
        }
        if (const auto* close = this->close()) {
            return makeClose(base + close->offset(), close->lineBytes());
        }
        return makeNoMatch();
    }

    Value value_;
};

[[nodiscard]] inline bool httpMultipartMarkerPrefixMatches(
    std::string_view input,
    std::string_view boundary) noexcept {
    const auto expectedSize = boundary.size() + 2;
    const auto compared = std::min(input.size(), expectedSize);
    for (std::size_t index = 0; index < compared; ++index) {
        const char expected = index < 2 ? '-' : boundary[index - 2];
        if (input[index] != expected) {
            return false;
        }
    }
    return true;
}

// Matches one delimiter line beginning at its leading "--". RFC 2046
// transport-padding is accepted after a regular delimiter or after the closing
// "--". A closing delimiter ending exactly at the current buffer boundary is
// complete only when the I/O owner has signalled end-of-input.
[[nodiscard]] inline HttpMultipartDelimiterResult httpMatchMultipartDelimiterLine(
    std::string_view input,
    const MultipartBoundary& boundary,
    bool inputFinished) noexcept {
    const auto value = boundary.value();
    const auto markerSize = value.size() + 2;
    if (!httpMultipartMarkerPrefixMatches(input, value)) {
        return HttpMultipartDelimiterResult::makeNoMatch();
    }
    if (input.size() < markerSize) {
        return inputFinished
            ? HttpMultipartDelimiterResult::makeNoMatch()
            : HttpMultipartDelimiterResult::makeNeedInput();
    }

    std::size_t cursor = markerSize;
    bool close = false;
    if (cursor < input.size() && input[cursor] == '-') {
        if (cursor + 1 >= input.size()) {
            return inputFinished
                ? HttpMultipartDelimiterResult::makeNoMatch()
                : HttpMultipartDelimiterResult::makeNeedInput();
        }
        if (input[cursor + 1] != '-') {
            return HttpMultipartDelimiterResult::makeNoMatch();
        }
        close = true;
        cursor += 2;
    }

    while (cursor < input.size() && (input[cursor] == ' ' || input[cursor] == '\t')) {
        ++cursor;
    }
    if (cursor == input.size()) {
        if (close && inputFinished) {
            return HttpMultipartDelimiterResult::makeClose(0, cursor);
        }
        return inputFinished
            ? HttpMultipartDelimiterResult::makeNoMatch()
            : HttpMultipartDelimiterResult::makeNeedInput();
    }
    if (input[cursor] != '\r') {
        return HttpMultipartDelimiterResult::makeNoMatch();
    }
    if (cursor + 1 >= input.size()) {
        return inputFinished
            ? HttpMultipartDelimiterResult::makeNoMatch()
            : HttpMultipartDelimiterResult::makeNeedInput();
    }
    if (input[cursor + 1] != '\n') {
        return HttpMultipartDelimiterResult::makeNoMatch();
    }
    return close
        ? HttpMultipartDelimiterResult::makeClose(0, cursor + 2)
        : HttpMultipartDelimiterResult::makePart(0, cursor + 2);
}

[[nodiscard]] inline HttpMultipartDelimiterResult httpFindInitialMultipartDelimiter(
    std::string_view input,
    const MultipartBoundary& boundary,
    bool inputFinished) noexcept {
    if (!input.empty() && input.front() == '-') {
        auto match = httpMatchMultipartDelimiterLine(input, boundary, inputFinished);
        if (match.noMatch() == nullptr) {
            return match;
        }
    }

    for (auto prefix = input.find("\r\n--");
         prefix != std::string_view::npos;
         prefix = input.find("\r\n--", prefix + 1)) {
        auto match = httpMatchMultipartDelimiterLine(
            input.substr(prefix + 2), boundary, inputFinished);
        if (match.noMatch() != nullptr) {
            continue;
        }
        return match.rebased(prefix + 2);
    }
    return HttpMultipartDelimiterResult::makeNoMatch();
}

[[nodiscard]] inline HttpMultipartDelimiterResult httpFindMultipartBodyDelimiter(
    std::string_view input,
    const MultipartBoundary& boundary,
    bool inputFinished) noexcept {
    for (auto prefix = input.find("\r\n--");
         prefix != std::string_view::npos;
         prefix = input.find("\r\n--", prefix + 1)) {
        auto match = httpMatchMultipartDelimiterLine(
            input.substr(prefix + 2), boundary, inputFinished);
        if (match.noMatch() != nullptr) {
            continue;
        }
        return match.rebased(prefix);
    }
    return HttpMultipartDelimiterResult::makeNoMatch();
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
            if (httpAsciiEqualsIgnoreCase(key, name)) {
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
    // Content-Disposition parameter names are case-insensitive (RFC 6266 section 4.1 /
    // RFC 2183), matching how httpParseMultipartBoundary treats the Content-Type
    // "boundary" parameter. Match "name"/"filename" the same way so a part using
    // e.g. `Name=` or `FileName=` is not spuriously rejected.
    const auto value = httpFindSemicolonParameterQuotedIgnoreCase(disposition, name);
    return value ? std::optional<std::string_view>(httpTrimQuotes(*value)) : std::nullopt;
}

[[nodiscard]] inline bool httpIsFormDataDisposition(std::string_view disposition) noexcept {
    const auto value = httpTrimOws(disposition);
    const auto semicolon = value.find(';');
    const auto type = httpTrimOws(semicolon == std::string_view::npos ? value : value.substr(0, semicolon));
    return httpAsciiEqualsIgnoreCase(type, "form-data");
}

enum class HttpMultipartPartHeaderParseError : std::uint8_t {
    kInvalidDisposition,
    kMissingName,
};

class HttpMultipartPartHeaderParseResult;

class HttpMultipartPartHeaders final {
public:
    [[nodiscard]] constexpr std::string_view name() const noexcept {
        return name_;
    }

    [[nodiscard]] constexpr std::string_view filename() const noexcept {
        return filename_;
    }

    [[nodiscard]] constexpr std::string_view contentType() const noexcept {
        return contentType_;
    }

private:
    friend class HttpMultipartPartHeaderParseResult;

    constexpr HttpMultipartPartHeaders(
        std::string_view name,
        std::string_view filename,
        std::string_view contentType) noexcept
        : name_(name), filename_(filename), contentType_(contentType) {}

    std::string_view name_;
    std::string_view filename_;
    std::string_view contentType_;
};

class HttpMultipartPartHeaderParseFailure final {
public:
    [[nodiscard]] constexpr HttpMultipartPartHeaderParseError
    error() const noexcept {
        return error_;
    }

private:
    friend class HttpMultipartPartHeaderParseResult;

    explicit constexpr HttpMultipartPartHeaderParseFailure(
        HttpMultipartPartHeaderParseError error) noexcept
        : error_(error) {}

    HttpMultipartPartHeaderParseError error_;
};

class HttpMultipartPartHeaderParseResult final {
public:
    [[nodiscard]] constexpr const HttpMultipartPartHeaders*
    headers() const noexcept {
        return std::get_if<HttpMultipartPartHeaders>(&value_);
    }

    [[nodiscard]] constexpr const HttpMultipartPartHeaderParseFailure*
    failure() const noexcept {
        return std::get_if<HttpMultipartPartHeaderParseFailure>(&value_);
    }

private:
    friend HttpMultipartPartHeaderParseResult httpParseMultipartPartHeaders(
        std::string_view) noexcept;

    using Value = std::variant<
        HttpMultipartPartHeaders,
        HttpMultipartPartHeaderParseFailure>;

    template <typename Result>
    explicit constexpr HttpMultipartPartHeaderParseResult(Result result) noexcept
        : value_(result) {}

    [[nodiscard]] static constexpr HttpMultipartPartHeaderParseResult
    makeHeaders(
        std::string_view name,
        std::string_view filename,
        std::string_view contentType) noexcept {
        return HttpMultipartPartHeaderParseResult(
            HttpMultipartPartHeaders(name, filename, contentType));
    }

    [[nodiscard]] static constexpr HttpMultipartPartHeaderParseResult
    makeFailure(HttpMultipartPartHeaderParseError error) noexcept {
        return HttpMultipartPartHeaderParseResult(
            HttpMultipartPartHeaderParseFailure(error));
    }

    Value value_;
};

enum class HttpMultipartBoundaryParseError : std::uint8_t {
    kInvalidContentType,
    kInvalidBoundary,
};

class HttpMultipartBoundaryParseFailure final {
public:
    [[nodiscard]] constexpr HttpMultipartBoundaryParseError
    error() const noexcept {
        return error_;
    }

private:
    friend class HttpMultipartBoundaryParseResult;

    explicit constexpr HttpMultipartBoundaryParseFailure(
        HttpMultipartBoundaryParseError error) noexcept
        : error_(error) {}

    HttpMultipartBoundaryParseError error_;
};

class HttpMultipartBoundaryParseResult final {
public:
    [[nodiscard]] constexpr const MultipartBoundary* boundary() const noexcept {
        return std::get_if<MultipartBoundary>(&value_);
    }

    [[nodiscard]] constexpr const HttpMultipartBoundaryParseFailure*
    failure() const noexcept {
        return std::get_if<HttpMultipartBoundaryParseFailure>(&value_);
    }

private:
    friend HttpMultipartBoundaryParseResult httpParseMultipartBoundary(std::string_view);

    using Value = std::variant<
        MultipartBoundary,
        HttpMultipartBoundaryParseFailure>;

    explicit HttpMultipartBoundaryParseResult(MultipartBoundary boundary)
        : value_(std::move(boundary)) {}

    explicit constexpr HttpMultipartBoundaryParseResult(
        HttpMultipartBoundaryParseFailure failure) noexcept
        : value_(failure) {}

    [[nodiscard]] static constexpr HttpMultipartBoundaryParseResult
    makeFailure(HttpMultipartBoundaryParseError error) noexcept {
        return HttpMultipartBoundaryParseResult(
            HttpMultipartBoundaryParseFailure(error));
    }

    Value value_;
};

[[nodiscard]] inline bool httpMimeTokenChar(char value) noexcept {
    const auto byte = static_cast<unsigned char>(value);
    if (byte <= 0x20 || byte >= 0x7F) {
        return false;
    }
    switch (value) {
        case '(':
        case ')':
        case '<':
        case '>':
        case '@':
        case ',':
        case ';':
        case ':':
        case '\\':
        case '"':
        case '/':
        case '[':
        case ']':
        case '?':
        case '=':
            return false;
        default:
            return true;
    }
}

[[nodiscard]] inline std::optional<MultipartBoundary>
httpDecodeMultipartBoundaryParameter(std::string_view parameter) {
    std::array<char, 70> decoded{};
    std::size_t size = 0;
    if (!parameter.empty() && parameter.front() == '"') {
        if (parameter.size() < 2 || parameter.back() != '"') {
            return std::nullopt;
        }
        parameter.remove_prefix(1);
        parameter.remove_suffix(1);
        for (std::size_t index = 0; index < parameter.size(); ++index) {
            char byte = parameter[index];
            if (byte == '\\') {
                if (++index >= parameter.size()) {
                    return std::nullopt;
                }
                byte = parameter[index];
            } else if (byte == '"') {
                return std::nullopt;
            }
            if (size >= decoded.size()) {
                return std::nullopt;
            }
            decoded[size++] = byte;
        }
    } else {
        if (parameter.empty()) {
            return std::nullopt;
        }
        for (const char byte : parameter) {
            if (!httpMimeTokenChar(byte) || size >= decoded.size()) {
                return std::nullopt;
            }
            decoded[size++] = byte;
        }
    }

    try {
        return MultipartBoundary(std::string_view(decoded.data(), size));
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
}

[[nodiscard]] inline HttpMultipartBoundaryParseResult httpParseMultipartBoundary(
    std::string_view contentType) {
    const auto mediaEnd = contentType.find(';');
    const auto mediaType = httpTrimOws(
        mediaEnd == std::string_view::npos ? contentType : contentType.substr(0, mediaEnd));
    if (!httpAsciiEqualsIgnoreCase(mediaType, "multipart/form-data")) {
        return HttpMultipartBoundaryParseResult::makeFailure(
            HttpMultipartBoundaryParseError::kInvalidContentType);
    }
    if (mediaEnd == std::string_view::npos) {
        return HttpMultipartBoundaryParseResult::makeFailure(
            HttpMultipartBoundaryParseError::kInvalidBoundary);
    }

    contentType.remove_prefix(mediaEnd + 1);
    std::optional<MultipartBoundary> boundary;
    bool repeated = false;
    httpVisitSemicolonParametersQuoted(
        contentType,
        [&boundary, &repeated](std::string_view key, std::string_view value) {
            if (!httpAsciiEqualsIgnoreCase(key, "boundary")) {
                return true;
            }
            if (boundary) {
                repeated = true;
                return false;
            }
            boundary = httpDecodeMultipartBoundaryParameter(value);
            return boundary.has_value();
        });
    if (!boundary || repeated) {
        return HttpMultipartBoundaryParseResult::makeFailure(
            HttpMultipartBoundaryParseError::kInvalidBoundary);
    }
    return HttpMultipartBoundaryParseResult(std::move(*boundary));
}

[[nodiscard]] inline HttpMultipartPartHeaderParseResult
httpParseMultipartPartHeaders(std::string_view headers) noexcept {
    const auto disposition = httpHeaderValueInBlock(headers, "Content-Disposition");
    if (!disposition || !httpIsFormDataDisposition(*disposition)) {
        return HttpMultipartPartHeaderParseResult::makeFailure(
            HttpMultipartPartHeaderParseError::kInvalidDisposition);
    }

    const auto name = httpDispositionParameter(*disposition, "name");
    if (!name) {
        return HttpMultipartPartHeaderParseResult::makeFailure(
            HttpMultipartPartHeaderParseError::kMissingName);
    }

    const auto filename = httpDispositionParameter(*disposition, "filename");
    const auto contentType = httpHeaderValueInBlock(headers, "Content-Type");
    return HttpMultipartPartHeaderParseResult::makeHeaders(
        *name,
        filename.value_or(std::string_view{}),
        contentType.value_or(std::string_view{}));
}

}  // namespace ruvia::detail
