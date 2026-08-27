#include "ruvia/http/MultipartParser.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/parser/MimeFieldGrammar.h"

namespace ruvia {
namespace {

[[nodiscard]] std::optional<MultipartBoundary> decodeMultipartBoundaryParameter(
    std::string_view parameter) {
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
            if (!detail::httpMimeTokenChar(byte) || size >= decoded.size()) {
                return std::nullopt;
            }
            decoded[size++] = byte;
        }
    }

    return MultipartBoundary::tryCreate(std::string_view(decoded.data(), size));
}

}  // namespace

MultipartBoundaryParseResult parseMultipartBoundary(std::string_view contentType) {
    const auto mediaEnd = contentType.find(';');
    const auto mediaType = detail::httpTrimOws(
        mediaEnd == std::string_view::npos ? contentType : contentType.substr(0, mediaEnd));
    if (!detail::httpAsciiEqualsIgnoreCase(mediaType, "multipart/form-data")) {
        return MultipartBoundaryParseResult::makeNotApplicable();
    }
    if (mediaEnd == std::string_view::npos) {
        return MultipartBoundaryParseResult::makeFailure();
    }

    std::optional<MultipartBoundary> boundary;
    detail::HttpMimeParameterNames parameterNames;
    const auto parameters = contentType.substr(mediaEnd + 1);
    std::size_t start = 0;
    while (start <= parameters.size()) {
        const auto end = detail::httpFindUnquotedDelimiter(parameters, start, ';');
        const auto parameter = detail::httpTrimOws(parameters.substr(start, end - start));
        std::string_view key;
        std::string_view value;
        if (!detail::httpParseMimeParameter(parameter, key, value) || !parameterNames.record(key)) {
            return MultipartBoundaryParseResult::makeFailure();
        }
        if (detail::httpAsciiEqualsIgnoreCase(key, "boundary")) {
            boundary = decodeMultipartBoundaryParameter(value);
            if (!boundary) {
                return MultipartBoundaryParseResult::makeFailure();
            }
        }

        if (end >= parameters.size()) {
            break;
        }
        start = end + 1;
    }
    if (!boundary) {
        return MultipartBoundaryParseResult::makeFailure();
    }
    return MultipartBoundaryParseResult(std::move(*boundary));
}

}  // namespace ruvia
