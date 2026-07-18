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
    noMatch() const & noexcept {
        return std::get_if<HttpMultipartDelimiterNoMatch>(&value_);
    }
    const HttpMultipartDelimiterNoMatch* noMatch() const && = delete;

    [[nodiscard]] constexpr const HttpMultipartDelimiterNeedInput*
    needInput() const & noexcept {
        return std::get_if<HttpMultipartDelimiterNeedInput>(&value_);
    }
    const HttpMultipartDelimiterNeedInput* needInput() const && = delete;

    [[nodiscard]] constexpr const HttpMultipartPartDelimiter*
    part() const & noexcept {
        return std::get_if<HttpMultipartPartDelimiter>(&value_);
    }
    const HttpMultipartPartDelimiter* part() const && = delete;

    [[nodiscard]] constexpr const HttpMultipartCloseDelimiter*
    close() const & noexcept {
        return std::get_if<HttpMultipartCloseDelimiter>(&value_);
    }
    const HttpMultipartCloseDelimiter* close() const && = delete;

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

template <HttpTemporaryOwningCharString Headers>
std::optional<std::string_view> httpHeaderValueInBlock(
    Headers&&,
    std::string_view) = delete;

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

template <HttpTemporaryOwningCharString Disposition>
std::optional<std::string_view> httpDispositionParameter(
    Disposition&&,
    std::string_view) = delete;

[[nodiscard]] inline bool httpIsFormDataDisposition(std::string_view disposition) noexcept {
    const auto value = httpTrimOws(disposition);
    const auto semicolon = value.find(';');
    const auto type = httpTrimOws(semicolon == std::string_view::npos ? value : value.substr(0, semicolon));
    return httpAsciiEqualsIgnoreCase(type, "form-data");
}

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
    [[nodiscard]] constexpr MultipartParseError parseError() const noexcept {
        return error_;
    }

private:
    friend class HttpMultipartPartHeaderParseResult;

    explicit constexpr HttpMultipartPartHeaderParseFailure(
        MultipartParseError error) noexcept
        : error_(error) {}

    MultipartParseError error_;
};

class HttpMultipartPartHeaderParseResult final {
public:
    [[nodiscard]] constexpr const HttpMultipartPartHeaders*
    headers() const & noexcept {
        return std::get_if<HttpMultipartPartHeaders>(&value_);
    }
    const HttpMultipartPartHeaders* headers() const && = delete;

    [[nodiscard]] constexpr const HttpMultipartPartHeaderParseFailure*
    failure() const & noexcept {
        return std::get_if<HttpMultipartPartHeaderParseFailure>(&value_);
    }
    const HttpMultipartPartHeaderParseFailure* failure() const && = delete;

private:
    friend HttpMultipartPartHeaderParseResult httpParseMultipartPartHeaders(
        std::string_view) noexcept;

    using Value = std::variant<
        HttpMultipartPartHeaders,
        HttpMultipartPartHeaderParseFailure>;

    explicit constexpr HttpMultipartPartHeaderParseResult(
        HttpMultipartPartHeaders headers) noexcept
        : value_(headers) {}

    explicit constexpr HttpMultipartPartHeaderParseResult(
        HttpMultipartPartHeaderParseFailure failure) noexcept
        : value_(failure) {}

    [[nodiscard]] static constexpr HttpMultipartPartHeaderParseResult
    makeHeaders(
        std::string_view name,
        std::string_view filename,
        std::string_view contentType) noexcept {
        return HttpMultipartPartHeaderParseResult(
            HttpMultipartPartHeaders(name, filename, contentType));
    }

    [[nodiscard]] static constexpr HttpMultipartPartHeaderParseResult
    makeFailure(MultipartParseError error) noexcept {
        return HttpMultipartPartHeaderParseResult(
            HttpMultipartPartHeaderParseFailure(error));
    }

    Value value_;
};

class HttpMultipartBoundaryNotApplicable final {
private:
    friend class HttpMultipartBoundaryParseResult;

    constexpr HttpMultipartBoundaryNotApplicable() noexcept = default;
};

class HttpMultipartBoundaryParseFailure final {
public:
    [[nodiscard]] HttpProtocolError protocolError() const noexcept {
        return HttpProtocolError(http_status::kBadRequest, "invalid multipart boundary");
    }

private:
    friend class HttpMultipartBoundaryParseResult;

    constexpr HttpMultipartBoundaryParseFailure() noexcept = default;
};

class HttpMultipartBoundaryParseResult final {
public:
    [[nodiscard]] constexpr const MultipartBoundary*
    boundary() const & noexcept {
        return std::get_if<MultipartBoundary>(&value_);
    }
    const MultipartBoundary* boundary() const && = delete;

    [[nodiscard]] constexpr const HttpMultipartBoundaryNotApplicable*
    notApplicable() const & noexcept {
        return std::get_if<HttpMultipartBoundaryNotApplicable>(&value_);
    }
    const HttpMultipartBoundaryNotApplicable*
    notApplicable() const && = delete;

    [[nodiscard]] constexpr const HttpMultipartBoundaryParseFailure*
    failure() const & noexcept {
        return std::get_if<HttpMultipartBoundaryParseFailure>(&value_);
    }
    const HttpMultipartBoundaryParseFailure* failure() const && = delete;

private:
    friend HttpMultipartBoundaryParseResult httpParseMultipartBoundary(std::string_view);

    using Value = std::variant<
        MultipartBoundary,
        HttpMultipartBoundaryNotApplicable,
        HttpMultipartBoundaryParseFailure>;

    explicit HttpMultipartBoundaryParseResult(MultipartBoundary boundary)
        : value_(std::move(boundary)) {}

    [[nodiscard]] static constexpr HttpMultipartBoundaryParseResult
    makeNotApplicable() noexcept {
        return HttpMultipartBoundaryParseResult(
            HttpMultipartBoundaryNotApplicable());
    }

    [[nodiscard]] static constexpr HttpMultipartBoundaryParseResult
    makeFailure() noexcept {
        return HttpMultipartBoundaryParseResult(
            HttpMultipartBoundaryParseFailure());
    }

    explicit constexpr HttpMultipartBoundaryParseResult(
        HttpMultipartBoundaryNotApplicable notApplicable) noexcept
        : value_(notApplicable) {}

    explicit constexpr HttpMultipartBoundaryParseResult(
        HttpMultipartBoundaryParseFailure failure) noexcept
        : value_(failure) {}

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

[[nodiscard]] inline bool httpValidMimeFieldName(
    std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char byte) {
        const auto character = static_cast<unsigned char>(byte);
        return character >= 33 && character <= 126 && byte != ':';
    });
}

[[nodiscard]] inline bool httpValidMimeFieldBody(
    std::string_view value) noexcept {
    return std::all_of(value.begin(), value.end(), [](char byte) {
        const auto character = static_cast<unsigned char>(byte);
        return character == '\t' ||
            (character >= 0x20 && character != 0x7F);
    });
}

[[nodiscard]] inline bool httpValidMimeParameterValue(
    std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    if (value.front() != '"') {
        return std::all_of(value.begin(), value.end(), httpMimeTokenChar);
    }
    if (value.size() < 2 || value.back() != '"') {
        return false;
    }

    const auto last = value.size() - 1;
    for (std::size_t index = 1; index < last; ++index) {
        const auto byte = static_cast<unsigned char>(value[index]);
        if (value[index] == '\\') {
            if (++index >= last) {
                return false;
            }
            const auto escaped = static_cast<unsigned char>(value[index]);
            if (escaped != '\t' && (escaped < 0x20 || escaped == 0x7F)) {
                return false;
            }
            continue;
        }
        if (value[index] == '"' || byte == 0 || byte == '\r' ||
            byte == '\n' || byte == 0x7F || (byte < 0x20 && byte != '\t')) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool httpParseMimeParameter(
    std::string_view parameter,
    std::string_view& name,
    std::string_view& value,
    bool strictEquals = true) noexcept {
    const auto equals = parameter.find('=');
    if (parameter.empty() || equals == std::string_view::npos) {
        return false;
    }

    const auto rawName = parameter.substr(0, equals);
    const auto rawValue = parameter.substr(equals + 1);
    name = httpTrimOws(rawName);
    value = httpTrimOws(rawValue);
    // Top-level HTTP media-type parameters forbid OWS around '=' (RFC 9110
    // section 5.6.6). MIME body-part structured fields retain RFC 822's
    // separator whitespace and opt out while sharing the remaining checks.
    return (!strictEquals ||
            (name.size() == rawName.size() && value.size() == rawValue.size())) &&
        !name.empty() &&
        std::all_of(name.begin(), name.end(), httpMimeTokenChar) &&
        httpValidMimeParameterValue(value);
}

template <HttpTemporaryOwningCharString Parameter>
bool httpParseMimeParameter(
    Parameter&&,
    std::string_view&,
    std::string_view&,
    bool = true) = delete;

class HttpMimeParameterNames final {
public:
    [[nodiscard]] bool record(std::string_view name) noexcept {
        for (std::size_t index = 0; index < size_; ++index) {
            if (httpAsciiEqualsIgnoreCase(names_[index], name)) {
                return false;
            }
        }
        // Parameter-heavy input is hostile in practice. A fixed bound keeps
        // duplicate detection allocation-free and its worst-case work constant.
        if (size_ == names_.size()) {
            return false;
        }
        names_[size_++] = name;
        return true;
    }

private:
    std::array<std::string_view, 64> names_{};
    std::size_t size_ = 0;
};

[[nodiscard]] inline bool httpValidMimeMediaType(
    std::string_view value) noexcept {
    const auto parameters = httpFindUnquotedDelimiter(value, 0, ';');
    const auto mediaType = httpTrimOws(value.substr(0, parameters));
    const auto slash = mediaType.find('/');
    if (slash == std::string_view::npos ||
        mediaType.find('/', slash + 1) != std::string_view::npos) {
        return false;
    }
    const auto type = mediaType.substr(0, slash);
    const auto subtype = mediaType.substr(slash + 1);
    if (type == "*" || subtype == "*" ||
        !std::all_of(type.begin(), type.end(), httpMimeTokenChar) ||
        !std::all_of(subtype.begin(), subtype.end(), httpMimeTokenChar) ||
        type.empty() || subtype.empty()) {
        return false;
    }
    if (parameters >= value.size()) {
        return true;
    }

    HttpMimeParameterNames parameterNames;
    std::size_t start = parameters + 1;
    while (start <= value.size()) {
        const auto end = httpFindUnquotedDelimiter(value, start, ';');
        const auto parameter = httpTrimOws(value.substr(start, end - start));
        std::string_view name;
        std::string_view parameterValue;
        if (!httpParseMimeParameter(
                parameter, name, parameterValue, false) ||
            !parameterNames.record(name)) {
            return false;
        }
        if (end >= value.size()) {
            return true;
        }
        start = end + 1;
    }
    return true;
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
        return HttpMultipartBoundaryParseResult::makeNotApplicable();
    }
    if (mediaEnd == std::string_view::npos) {
        return HttpMultipartBoundaryParseResult::makeFailure();
    }

    std::optional<MultipartBoundary> boundary;
    HttpMimeParameterNames parameterNames;
    const auto parameters = contentType.substr(mediaEnd + 1);
    std::size_t start = 0;
    while (start <= parameters.size()) {
        const auto end = httpFindUnquotedDelimiter(parameters, start, ';');
        const auto parameter = httpTrimOws(parameters.substr(start, end - start));
        std::string_view key;
        std::string_view value;
        if (!httpParseMimeParameter(parameter, key, value) ||
            !parameterNames.record(key)) {
            return HttpMultipartBoundaryParseResult::makeFailure();
        }
        if (httpAsciiEqualsIgnoreCase(key, "boundary")) {
            boundary = httpDecodeMultipartBoundaryParameter(value);
            if (!boundary) {
                return HttpMultipartBoundaryParseResult::makeFailure();
            }
        }

        if (end >= parameters.size()) {
            break;
        }
        start = end + 1;
    }
    if (!boundary) {
        return HttpMultipartBoundaryParseResult::makeFailure();
    }
    return HttpMultipartBoundaryParseResult(std::move(*boundary));
}

[[nodiscard]] inline HttpMultipartPartHeaderParseResult
httpParseMultipartPartHeaders(std::string_view headers) noexcept {
    std::optional<std::string_view> disposition;
    std::optional<std::string_view> contentType;
    auto remainingHeaders = headers;
    while (!remainingHeaders.empty()) {
        const auto lineEnd = remainingHeaders.find("\r\n");
        const auto line = lineEnd == std::string_view::npos
            ? remainingHeaders
            : remainingHeaders.substr(0, lineEnd);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos ||
            !httpValidMimeFieldName(line.substr(0, colon)) ||
            !httpValidMimeFieldBody(line.substr(colon + 1))) {
            return HttpMultipartPartHeaderParseResult::makeFailure(
                MultipartParseError::kInvalidPartHeaders);
        }
        const auto key = line.substr(0, colon);
        const auto value = httpTrimOws(line.substr(colon + 1));
        if (httpAsciiEqualsIgnoreCase(key, "Content-Disposition")) {
            if (disposition) {
                return HttpMultipartPartHeaderParseResult::makeFailure(
                    MultipartParseError::kInvalidContentDisposition);
            }
            disposition = value;
        } else if (httpAsciiEqualsIgnoreCase(key, "Content-Type")) {
            if (contentType || !httpValidMimeMediaType(value)) {
                return HttpMultipartPartHeaderParseResult::makeFailure(
                    MultipartParseError::kInvalidPartHeaders);
            }
            contentType = value;
        }
        if (lineEnd == std::string_view::npos) {
            break;
        }
        remainingHeaders.remove_prefix(lineEnd + 2);
    }

    if (!disposition || !httpIsFormDataDisposition(*disposition)) {
        return HttpMultipartPartHeaderParseResult::makeFailure(
            MultipartParseError::kInvalidContentDisposition);
    }

    const auto parameters = disposition->find(';');
    if (parameters == std::string_view::npos) {
        return HttpMultipartPartHeaderParseResult::makeFailure(
            MultipartParseError::kMissingFieldName);
    }

    std::optional<std::string_view> name;
    std::optional<std::string_view> filename;
    HttpMimeParameterNames parameterNames;
    auto remaining = disposition->substr(parameters + 1);
    std::size_t start = 0;
    while (start <= remaining.size()) {
        const auto end = httpFindUnquotedDelimiter(remaining, start, ';');
        const auto parameter = httpTrimOws(remaining.substr(start, end - start));
        std::string_view key;
        std::string_view value;
        if (!httpParseMimeParameter(parameter, key, value, false) ||
            !parameterNames.record(key)) {
            return HttpMultipartPartHeaderParseResult::makeFailure(
                MultipartParseError::kInvalidContentDisposition);
        }
        // RFC 7578 section 4.2 forbids RFC 5987's filename* parameter in
        // multipart/form-data; accepting and ignoring it loses the filename.
        if (httpAsciiEqualsIgnoreCase(key, "filename*")) {
            return HttpMultipartPartHeaderParseResult::makeFailure(
                MultipartParseError::kInvalidContentDisposition);
        }

        const auto decoded = httpTrimQuotes(value);
        if (httpAsciiEqualsIgnoreCase(key, "name")) {
            name = decoded;
        } else if (httpAsciiEqualsIgnoreCase(key, "filename")) {
            filename = decoded;
        }

        if (end >= remaining.size()) {
            break;
        }
        start = end + 1;
    }

    if (!name) {
        return HttpMultipartPartHeaderParseResult::makeFailure(
            MultipartParseError::kMissingFieldName);
    }

    return HttpMultipartPartHeaderParseResult::makeHeaders(
        *name,
        filename.value_or(std::string_view{}),
        contentType.value_or(std::string_view{}));
}

}  // namespace ruvia::detail
