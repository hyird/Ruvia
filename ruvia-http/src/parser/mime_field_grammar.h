#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

#include "ruvia/http/detail/field/header_token_utils.h"

// The MIME entity grammar multipart bodies are validated against (RFC 2045
// sections 5.1 and 5.3): which characters form a token, what a field name, field
// body, parameter and media type may contain, and the duplicate-parameter guard
// a media type needs.

namespace ruvia::detail {

[[nodiscard]] inline bool http_mime_token_char(char value) noexcept {
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

[[nodiscard]] inline bool http_valid_mime_field_name(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    return std::ranges::all_of(value, [](char byte) {
        const auto character = static_cast<unsigned char>(byte);
        return character >= 33 && character <= 126 && byte != ':';
    });
}

[[nodiscard]] inline bool http_valid_mime_field_body(std::string_view value) noexcept {
    return std::ranges::all_of(value, [](char byte) {
        const auto character = static_cast<unsigned char>(byte);
        return character == '\t' || (character >= 0x20 && character != 0x7F);
    });
}

[[nodiscard]] inline bool http_valid_mime_parameter_value(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    if (value.front() != '"') {
        return std::ranges::all_of(value, http_mime_token_char);
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
        if (value[index] == '"' || byte == 0 || byte == '\r' || byte == '\n' || byte == 0x7F ||
            (byte < 0x20 && byte != '\t')) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool http_parse_mime_parameter(std::string_view parameter, std::string_view& name,
    std::string_view& value, bool strict_equals = true) noexcept {
    const auto equals = parameter.find('=');
    if (parameter.empty() || equals == std::string_view::npos) {
        return false;
    }

    const auto raw_name = parameter.substr(0, equals);
    const auto raw_value = parameter.substr(equals + 1);
    name = http_trim_ows(raw_name);
    value = http_trim_ows(raw_value);
    // Top-level HTTP media-type parameters forbid OWS around '=' (RFC 9110
    // section 5.6.6). MIME body-part structured fields retain RFC 822's
    // separator whitespace and opt out while sharing the remaining checks.
    return (!strict_equals || (name.size() == raw_name.size() && value.size() == raw_value.size())) &&
           !name.empty() && std::ranges::all_of(name, http_mime_token_char) &&
           http_valid_mime_parameter_value(value);
}

template <http_temporary_owning_char_string parameter_type>
bool http_parse_mime_parameter(
    parameter_type&&, std::string_view&, std::string_view&, bool = true) = delete;

class http_mime_parameter_names final {
public:
    [[nodiscard]] bool record(std::string_view name) noexcept {
        for (std::size_t index = 0; index < size_; ++index) {
            if (http_ascii_equals_ignore_case(names_[index], name)) {
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

[[nodiscard]] inline bool http_valid_mime_media_type(std::string_view value) noexcept {
    const auto parameters = http_find_unquoted_delimiter(value, 0, ';');
    const auto media_type = http_trim_ows(value.substr(0, parameters));
    const auto slash = media_type.find('/');
    if (slash == std::string_view::npos ||
        media_type.find('/', slash + 1) != std::string_view::npos) {
        return false;
    }
    const auto type = media_type.substr(0, slash);
    const auto subtype = media_type.substr(slash + 1);
    if (type == "*" || subtype == "*" || !std::ranges::all_of(type, http_mime_token_char) ||
        !std::ranges::all_of(subtype, http_mime_token_char) || type.empty() || subtype.empty()) {
        return false;
    }
    http_mime_parameter_names parameter_names;
    return http_all_parameters(value, [&parameter_names](std::string_view parameter) noexcept {
        std::string_view name;
        std::string_view parameter_value;
        return http_parse_mime_parameter(parameter, name, parameter_value, false) &&
               parameter_names.record(name);
    });
}

}  // namespace ruvia::detail
