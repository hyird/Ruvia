#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/MultipartParser.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/parser/MimeFieldGrammar.h"

// Extracting the boundary from a multipart Content-Type (RFC 2046 section
// 5.1.1): the media type must be multipart/form-data, the boundary parameter
// must be present exactly once, and its value must decode to 1-70 legal bytes.

namespace ruvia::detail {

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

}  // namespace ruvia::detail
