#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/multipart_parser.h"

#include "parser/mime_field_grammar.h"

namespace ruvia {
namespace {

[[nodiscard]] std::optional<multipart_boundary> decode_multipart_boundary_parameter(
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
            if (!detail::http_mime_token_char(byte) || size >= decoded.size()) {
                return std::nullopt;
            }
            decoded[size++] = byte;
        }
    }

    return multipart_boundary::try_create(std::string_view(decoded.data(), size));
}

}  // namespace

multipart_boundary_parse_result parse_multipart_boundary(std::string_view content_type_value) {
    const auto media_end = content_type_value.find(';');
    const auto media_type = detail::http_trim_ows(
        media_end == std::string_view::npos ? content_type_value : content_type_value.substr(0, media_end));
    if (!detail::http_ascii_equals_ignore_case(media_type, "multipart/form-data")) {
        return multipart_boundary_parse_result::make_not_applicable();
    }
    if (media_end == std::string_view::npos) {
        return multipart_boundary_parse_result::make_failure();
    }

    std::optional<multipart_boundary> boundary;
    detail::http_mime_parameter_names parameter_names;
    const auto parameters = content_type_value.substr(media_end + 1);
    std::size_t start = 0;
    while (start <= parameters.size()) {
        const auto end = detail::http_find_unquoted_delimiter(parameters, start, ';');
        const auto parameter = detail::http_trim_ows(parameters.substr(start, end - start));
        std::string_view key;
        std::string_view value;
        if (!detail::http_parse_mime_parameter(parameter, key, value) || !parameter_names.record(key)) {
            return multipart_boundary_parse_result::make_failure();
        }
        if (detail::http_ascii_equals_ignore_case(key, "boundary")) {
            boundary = decode_multipart_boundary_parameter(value);
            if (!boundary) {
                return multipart_boundary_parse_result::make_failure();
            }
        }

        if (end >= parameters.size()) {
            break;
        }
        start = end + 1;
    }
    if (!boundary) {
        return multipart_boundary_parse_result::make_failure();
    }
    return multipart_boundary_parse_result(std::move(*boundary));
}

}  // namespace ruvia
