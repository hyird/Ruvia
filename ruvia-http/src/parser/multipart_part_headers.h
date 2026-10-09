#pragma once

#include <optional>
#include <string_view>
#include <variant>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/multipart_parser.h"

#include "parser/mime_field_grammar.h"

// One part's header block: field lookup inside the block, the Content-Disposition
// parameters a form-data part must carry (RFC 7578 section 4.2), and the parse
// result exposing its name, filename and content type -- or the exact reason the
// block is not a usable form-data part.

namespace ruvia::detail {

[[nodiscard]] inline std::optional<std::string_view> http_header_value_in_block(
    std::string_view headers, std::string_view name) noexcept {
    std::optional<std::string_view> result;
    while (!headers.empty()) {
        const auto line_end = headers.find("\r\n");
        const auto line = line_end == std::string_view::npos ? headers : headers.substr(0, line_end);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos) {
            const auto key = http_trim_ows(line.substr(0, colon));
            if (http_ascii_equals_ignore_case(key, name)) {
                result = http_trim_ows(line.substr(colon + 1));
            }
        }

        if (line_end == std::string_view::npos) {
            break;
        }
        headers.remove_prefix(line_end + 2);
    }

    return result;
}

template <http_temporary_owning_char_string headers_type>
std::optional<std::string_view> http_header_value_in_block(headers_type&&, std::string_view) = delete;

[[nodiscard]] inline std::optional<std::string_view> http_disposition_parameter(
    std::string_view disposition, std::string_view name) noexcept {
    // Content-Disposition parameter names are case-insensitive (RFC 6266 section 4.1 /
    // RFC 2183), matching how parse_multipart_boundary treats the Content-Type
    // "boundary" parameter. Match "name"/"filename" the same way so a part using
    // e.g. `Name=` or `FileName=` is not spuriously rejected.
    const auto value = http_find_semicolon_parameter_quoted_ignore_case(disposition, name);
    if (!value) {
        return std::nullopt;
    }
    return http_trim_quotes(*value);
}

template <http_temporary_owning_char_string disposition_type>
std::optional<std::string_view> http_disposition_parameter(disposition_type&&, std::string_view) = delete;

[[nodiscard]] inline bool http_is_form_data_disposition(std::string_view disposition) noexcept {
    const auto value = http_trim_ows(disposition);
    const auto semicolon = value.find(';');
    const auto type =
        http_trim_ows(semicolon == std::string_view::npos ? value : value.substr(0, semicolon));
    return http_ascii_equals_ignore_case(type, "form-data");
}

class http_multipart_part_header_parse_result;

class http_multipart_part_headers final {
public:
    [[nodiscard]] constexpr std::string_view name() const noexcept {
        return name_;
    }

    [[nodiscard]] constexpr std::string_view filename() const noexcept {
        return filename_;
    }

    [[nodiscard]] constexpr bool has_filename() const noexcept {
        return filename_present_;
    }

    [[nodiscard]] constexpr std::string_view content_type() const noexcept {
        return content_type_;
    }

private:
    friend class http_multipart_part_header_parse_result;

    constexpr http_multipart_part_headers(std::string_view name, std::string_view filename,
        std::string_view content_type_value, bool filename_present) noexcept
        : name_(name),
          filename_(filename),
          content_type_(content_type_value),
          filename_present_(filename_present) {}

    std::string_view name_;
    std::string_view filename_;
    std::string_view content_type_;
    bool filename_present_{false};
};

class http_multipart_part_header_parse_failure final {
public:
    [[nodiscard]] constexpr multipart_parse_error parse_error() const noexcept {
        return error_;
    }

private:
    friend class http_multipart_part_header_parse_result;

    explicit constexpr http_multipart_part_header_parse_failure(multipart_parse_error error) noexcept
        : error_(error) {}

    multipart_parse_error error_;
};

class http_multipart_part_header_parse_result final {
public:
    [[nodiscard]] constexpr const http_multipart_part_headers* headers() const& noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    const http_multipart_part_headers* headers() const&& = delete;

    [[nodiscard]] constexpr const http_multipart_part_header_parse_failure* failure() const& noexcept {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    const http_multipart_part_header_parse_failure* failure() const&& = delete;

private:
    friend http_multipart_part_header_parse_result http_parse_multipart_part_headers(
        std::string_view) noexcept;

    using value_type = std::variant<http_multipart_part_headers, http_multipart_part_header_parse_failure>;

    explicit constexpr http_multipart_part_header_parse_result(http_multipart_part_headers headers) noexcept
        : value_(headers) {}

    explicit constexpr http_multipart_part_header_parse_result(
        http_multipart_part_header_parse_failure failure) noexcept
        : value_(failure) {}

    [[nodiscard]] static constexpr http_multipart_part_header_parse_result make_headers(
        std::string_view name, std::string_view filename, std::string_view content_type_value,
        bool filename_present) noexcept {
        return http_multipart_part_header_parse_result(
            http_multipart_part_headers(name, filename, content_type_value, filename_present));
    }

    [[nodiscard]] static constexpr http_multipart_part_header_parse_result make_failure(
        multipart_parse_error error) noexcept {
        return http_multipart_part_header_parse_result(http_multipart_part_header_parse_failure(error));
    }

    value_type value_;
};

[[nodiscard]] inline http_multipart_part_header_parse_result http_parse_multipart_part_headers(
    std::string_view headers) noexcept {
    std::optional<std::string_view> disposition;
    std::optional<std::string_view> content_type;
    auto remaining_headers = headers;
    while (!remaining_headers.empty()) {
        const auto line_end = remaining_headers.find("\r\n");
        const auto line = line_end == std::string_view::npos ? remaining_headers
                                                             : remaining_headers.substr(0, line_end);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || !http_valid_mime_field_name(line.substr(0, colon)) ||
            !http_valid_mime_field_body(line.substr(colon + 1))) {
            return http_multipart_part_header_parse_result::make_failure(
                multipart_parse_error::invalid_part_headers);
        }
        const auto key = line.substr(0, colon);
        const auto value = http_trim_ows(line.substr(colon + 1));
        if (http_ascii_equals_ignore_case(key, "Content-Disposition")) {
            if (disposition) {
                return http_multipart_part_header_parse_result::make_failure(
                    multipart_parse_error::invalid_content_disposition);
            }
            disposition = value;
        } else if (http_ascii_equals_ignore_case(key, "Content-Type")) {
            if (content_type || !http_valid_mime_media_type(value)) {
                return http_multipart_part_header_parse_result::make_failure(
                    multipart_parse_error::invalid_part_headers);
            }
            content_type = value;
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        remaining_headers.remove_prefix(line_end + 2);
    }

    if (!disposition || !http_is_form_data_disposition(*disposition)) {
        return http_multipart_part_header_parse_result::make_failure(
            multipart_parse_error::invalid_content_disposition);
    }

    const auto parameters = disposition->find(';');
    if (parameters == std::string_view::npos) {
        return http_multipart_part_header_parse_result::make_failure(
            multipart_parse_error::missing_field_name);
    }

    std::optional<std::string_view> name;
    std::optional<std::string_view> filename;
    http_mime_parameter_names parameter_names;
    auto remaining = disposition->substr(parameters + 1);
    std::size_t start = 0;
    while (start <= remaining.size()) {
        const auto end = http_find_unquoted_delimiter(remaining, start, ';');
        const auto parameter = http_trim_ows(remaining.substr(start, end - start));
        std::string_view key;
        std::string_view value;
        if (!http_parse_mime_parameter(parameter, key, value, false) || !parameter_names.record(key)) {
            return http_multipart_part_header_parse_result::make_failure(
                multipart_parse_error::invalid_content_disposition);
        }
        // RFC 7578 section 4.2 forbids RFC 5987's filename* parameter in
        // multipart/form-data; accepting and ignoring it loses the filename.
        if (http_ascii_equals_ignore_case(key, "filename*")) {
            return http_multipart_part_header_parse_result::make_failure(
                multipart_parse_error::invalid_content_disposition);
        }

        const auto decoded = http_trim_quotes(value);
        if (http_ascii_equals_ignore_case(key, "name")) {
            name = decoded;
        } else if (http_ascii_equals_ignore_case(key, "filename")) {
            filename = decoded;
        }

        if (end >= remaining.size()) {
            break;
        }
        start = end + 1;
    }

    if (!name) {
        return http_multipart_part_header_parse_result::make_failure(
            multipart_parse_error::missing_field_name);
    }

    return http_multipart_part_header_parse_result::make_headers(*name,
        filename.value_or(std::string_view{}), content_type.value_or(std::string_view{}),
        filename.has_value());
}

}  // namespace ruvia::detail
