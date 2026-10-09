#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory_resource>
#include <string>
#include <string_view>
#include <system_error>

#include "ruvia/http/http_response_file.h"

namespace ruvia::detail {

// URL index keys and extension policy use UTF-8 on every platform rather than
// the system narrow encoding of filesystem::path::generic_string<char>().
[[nodiscard]] inline std::pmr::u8string static_file_utf8_path(
    const std::filesystem::path& path, std::pmr::memory_resource* resource) {
    return path.generic_string<char8_t, std::char_traits<char8_t>,
        std::pmr::polymorphic_allocator<char8_t>>(
        std::pmr::polymorphic_allocator<char8_t>(resource));
}

template <typename char_value_type>
[[nodiscard]] inline std::basic_string_view<char_value_type> static_file_extension(
    std::basic_string_view<char_value_type> path) noexcept {
    for (std::size_t i = path.size(); i > 0; --i) {
        const auto c = path[i - 1];
        if (c == static_cast<char_value_type>('/') || c == static_cast<char_value_type>('\\')) {
            return {};
        }
        if (c == static_cast<char_value_type>('.')) {
            return path.substr(i - 1);
        }
    }
    return {};
}

template <typename char_value_type>
[[nodiscard]] inline bool static_file_extension_equals(
    std::basic_string_view<char_value_type> extension, std::string_view expected) noexcept {
    if (extension.size() != expected.size()) {
        return false;
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        auto c = extension[i];
        if (c >= static_cast<char_value_type>('A') && c <= static_cast<char_value_type>('Z')) {
            c = static_cast<char_value_type>(c + static_cast<char_value_type>('a' - 'A'));
        }
        if (c != static_cast<char_value_type>(expected[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline std::pmr::string lower_static_file_extension(
    const std::filesystem::path& path, std::pmr::memory_resource* resource) {
    // On Windows the native path uses wchar_t. Narrowing those code units one
    // by one can alias unrelated Unicode extensions to an ASCII allow-listed
    // type (for example U+0168 has the same low byte as 'h'). Convert the full
    // path to UTF-8 first so extension policy and MIME lookup compare the
    // actual filename bytes on every platform.
    const auto utf8_path = static_file_utf8_path(path, resource);
    const auto source_value = static_file_extension(std::u8string_view(utf8_path.data(), utf8_path.size()));
    if (source_value.empty()) {
        return std::pmr::string(resource);
    }
    std::pmr::string extension(resource);
    extension.reserve(source_value.size());
    for (const auto c : source_value) {
        auto out = c;
        if (out >= static_cast<char8_t>('A') && out <= static_cast<char8_t>('Z')) {
            out = static_cast<char8_t>(out + static_cast<char8_t>('a' - 'A'));
        }
        extension.push_back(static_cast<char>(out));
    }
    return extension;
}

template <typename char_value_type>
[[nodiscard]] inline std::string_view guess_static_file_content_type_from_path_view(
    std::basic_string_view<char_value_type> path) noexcept {
    const auto extension = static_file_extension(path);
    if (static_file_extension_equals(extension, ".html") ||
        static_file_extension_equals(extension, ".htm")) {
        return "text/html; charset=utf-8";
    }
    if (static_file_extension_equals(extension, ".css")) {
        return "text/css; charset=utf-8";
    }
    if (static_file_extension_equals(extension, ".js") ||
        static_file_extension_equals(extension, ".mjs")) {
        return "text/javascript; charset=utf-8";
    }
    if (static_file_extension_equals(extension, ".json")) {
        return "application/json; charset=utf-8";
    }
    if (static_file_extension_equals(extension, ".txt") ||
        static_file_extension_equals(extension, ".log")) {
        return "text/plain; charset=utf-8";
    }
    if (static_file_extension_equals(extension, ".png")) {
        return "image/png";
    }
    if (static_file_extension_equals(extension, ".jpg") ||
        static_file_extension_equals(extension, ".jpeg")) {
        return "image/jpeg";
    }
    if (static_file_extension_equals(extension, ".gif")) {
        return "image/gif";
    }
    if (static_file_extension_equals(extension, ".svg")) {
        return "image/svg+xml";
    }
    if (static_file_extension_equals(extension, ".wasm")) {
        return "application/wasm";
    }
    return "application/octet-stream";
}

[[nodiscard]] inline std::string_view guess_static_file_content_type(
    const std::filesystem::path& path) noexcept {
    return guess_static_file_content_type_from_path_view(
        std::basic_string_view<std::filesystem::path::value_type>(path.native()));
}

inline void append_static_file_unsigned(std::pmr::string& output, std::uint64_t value) {
    std::array<char, 32> buffer;
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec == std::errc{}) {
        output.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
    }
}

[[nodiscard]] inline std::pmr::string make_static_file_snapshot_etag(
    std::pmr::memory_resource* resource, std::uint64_t size, std::uint64_t modified_token,
    http_response_file_identity identity) {
    std::pmr::string output(resource);
    output.reserve(128);
    output.push_back('"');
    append_static_file_unsigned(output, size);
    output.push_back('-');
    append_static_file_unsigned(output, modified_token);
    for (const auto word : identity.words()) {
        output.push_back('-');
        append_static_file_unsigned(output, word);
    }
    output.push_back('"');
    return output;
}

}  // namespace ruvia::detail
