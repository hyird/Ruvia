#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

// The MIME entity grammar multipart bodies are validated against (RFC 2045
// sections 5.1 and 5.3): which characters form a token, what a field name, field
// body, parameter and media type may contain, and the duplicate-parameter guard
// a media type needs.

namespace ruvia::detail {

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
    return std::ranges::all_of(value, [](char byte) {
        const auto character = static_cast<unsigned char>(byte);
        return character >= 33 && character <= 126 && byte != ':';
    });
}

[[nodiscard]] inline bool httpValidMimeFieldBody(
    std::string_view value) noexcept {
    return std::ranges::all_of(value, [](char byte) {
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
        return std::ranges::all_of(value, httpMimeTokenChar);
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
        std::ranges::all_of(name, httpMimeTokenChar) &&
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
        !std::ranges::all_of(type, httpMimeTokenChar) ||
        !std::ranges::all_of(subtype, httpMimeTokenChar) ||
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

}  // namespace ruvia::detail
