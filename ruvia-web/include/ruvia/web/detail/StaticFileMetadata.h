#pragma once

#include "ruvia/http/detail/NativePath.h"

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <memory_resource>
#include <string>
#include <string_view>
#include <system_error>

namespace ruvia::detail {

template <typename Char>
[[nodiscard]] inline std::basic_string_view<Char> staticFileExtension(
    std::basic_string_view<Char> path) noexcept {
    for (std::size_t i = path.size(); i > 0; --i) {
        const auto c = path[i - 1];
        if (c == static_cast<Char>('/') || c == static_cast<Char>('\\')) {
            return {};
        }
        if (c == static_cast<Char>('.')) {
            return path.substr(i - 1);
        }
    }
    return {};
}

template <typename Char>
[[nodiscard]] inline bool staticFileExtensionEquals(
    std::basic_string_view<Char> extension,
    std::string_view expected) noexcept {
    if (extension.size() != expected.size()) {
        return false;
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        auto c = extension[i];
        if (c >= static_cast<Char>('A') && c <= static_cast<Char>('Z')) {
            c = static_cast<Char>(c + static_cast<Char>('a' - 'A'));
        }
        if (c != static_cast<Char>(expected[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline std::pmr::string lowerStaticFileExtension(
    const std::filesystem::path& path,
    std::pmr::memory_resource* resource) {
    const auto source = staticFileExtension(httpNativePathView(path));
    if (source.empty()) {
        return std::pmr::string(resource);
    }
    std::pmr::string extension(resource);
    extension.reserve(source.size());
    for (const auto c : source) {
        auto out = c;
        if (out >= static_cast<HttpNativePathChar>('A') &&
            out <= static_cast<HttpNativePathChar>('Z')) {
            out = static_cast<HttpNativePathChar>(
                out + static_cast<HttpNativePathChar>('a' - 'A'));
        }
        extension.push_back(static_cast<char>(out));
    }
    return extension;
}

template <typename Char>
[[nodiscard]] inline std::string_view guessStaticFileContentTypeFromPathView(
    std::basic_string_view<Char> path) noexcept {
    const auto extension = staticFileExtension(path);
    if (staticFileExtensionEquals(extension, ".html") ||
        staticFileExtensionEquals(extension, ".htm")) {
        return "text/html; charset=utf-8";
    }
    if (staticFileExtensionEquals(extension, ".css")) {
        return "text/css; charset=utf-8";
    }
    if (staticFileExtensionEquals(extension, ".js") ||
        staticFileExtensionEquals(extension, ".mjs")) {
        return "text/javascript; charset=utf-8";
    }
    if (staticFileExtensionEquals(extension, ".json")) {
        return "application/json; charset=utf-8";
    }
    if (staticFileExtensionEquals(extension, ".txt") ||
        staticFileExtensionEquals(extension, ".log")) {
        return "text/plain; charset=utf-8";
    }
    if (staticFileExtensionEquals(extension, ".png")) {
        return "image/png";
    }
    if (staticFileExtensionEquals(extension, ".jpg") ||
        staticFileExtensionEquals(extension, ".jpeg")) {
        return "image/jpeg";
    }
    if (staticFileExtensionEquals(extension, ".gif")) {
        return "image/gif";
    }
    if (staticFileExtensionEquals(extension, ".svg")) {
        return "image/svg+xml";
    }
    if (staticFileExtensionEquals(extension, ".wasm")) {
        return "application/wasm";
    }
    return "application/octet-stream";
}

[[nodiscard]] inline std::string_view guessStaticFileContentType(
    const std::filesystem::path& path) noexcept {
    return guessStaticFileContentTypeFromPathView(httpNativePathView(path));
}

inline void appendStaticFileUnsigned(std::pmr::string& output, std::uint64_t value) {
    std::array<char, 32> buffer;
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec == std::errc{}) {
        output.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
    }
}

[[nodiscard]] inline std::pmr::string makeStaticFileEtag(
    std::pmr::memory_resource* resource,
    std::uint64_t size,
    std::filesystem::file_time_type modified) {
    std::pmr::string output(resource);
    output.reserve(43);
    output.push_back('"');
    appendStaticFileUnsigned(output, size);
    output.push_back('-');
    appendStaticFileUnsigned(
        output,
        static_cast<std::uint64_t>(modified.time_since_epoch().count()));
    output.push_back('"');
    return output;
}

[[nodiscard]] inline std::time_t staticFileTimeToTimeT(
    std::filesystem::file_time_type value) noexcept {
    const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        value - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::system_clock::to_time_t(systemTime);
}

}  // namespace ruvia::detail
