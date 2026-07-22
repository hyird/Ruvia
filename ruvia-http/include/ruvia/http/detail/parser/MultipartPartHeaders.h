#pragma once

#include <optional>
#include <string_view>
#include <variant>

#include "ruvia/http/MultipartParser.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/parser/MimeFieldGrammar.h"

// One part's header block: field lookup inside the block, the Content-Disposition
// parameters a form-data part must carry (RFC 7578 section 4.2), and the parse
// result exposing its name, filename and content type -- or the exact reason the
// block is not a usable form-data part.

namespace ruvia::detail {

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
