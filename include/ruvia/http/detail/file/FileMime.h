#pragma once

#include <filesystem>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/memory/MemoryPool.h"

namespace ruvia::detail {

template <typename Char>
[[nodiscard]] inline std::basic_string_view<Char> httpFileExtension(
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
[[nodiscard]] inline bool httpExtensionEquals(
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

[[nodiscard]] inline std::pmr::string httpLowerFileExtension(
    const std::filesystem::path& path,
    std::pmr::memory_resource* resource = ProcessMemory::instance().upstreamResource()) {
    const auto native = std::basic_string_view<std::filesystem::path::value_type>(
        path.native().data(),
        path.native().size());
    const auto source = httpFileExtension(native);
    if (source.empty()) {
        return std::pmr::string(resource);
    }
    std::pmr::string extension(resource);
    extension.reserve(source.size());
    for (const auto c : source) {
        auto out = c;
        if (out >= static_cast<std::filesystem::path::value_type>('A') &&
            out <= static_cast<std::filesystem::path::value_type>('Z')) {
            out = static_cast<std::filesystem::path::value_type>(
                out + static_cast<std::filesystem::path::value_type>('a' - 'A'));
        }
        extension.push_back(static_cast<char>(out));
    }
    return extension;
}

[[nodiscard]] inline std::string_view httpGuessContentType(const std::filesystem::path& path) {
    const auto native = std::basic_string_view<std::filesystem::path::value_type>(
        path.native().data(),
        path.native().size());
    const auto extension = httpFileExtension(native);
    if (httpExtensionEquals(extension, ".html") || httpExtensionEquals(extension, ".htm")) {
        return "text/html; charset=utf-8";
    }
    if (httpExtensionEquals(extension, ".css")) {
        return "text/css; charset=utf-8";
    }
    if (httpExtensionEquals(extension, ".js") || httpExtensionEquals(extension, ".mjs")) {
        return "text/javascript; charset=utf-8";
    }
    if (httpExtensionEquals(extension, ".json")) {
        return "application/json; charset=utf-8";
    }
    if (httpExtensionEquals(extension, ".txt") || httpExtensionEquals(extension, ".log")) {
        return "text/plain; charset=utf-8";
    }
    if (httpExtensionEquals(extension, ".png")) {
        return "image/png";
    }
    if (httpExtensionEquals(extension, ".jpg") || httpExtensionEquals(extension, ".jpeg")) {
        return "image/jpeg";
    }
    if (httpExtensionEquals(extension, ".gif")) {
        return "image/gif";
    }
    if (httpExtensionEquals(extension, ".svg")) {
        return "image/svg+xml";
    }
    if (httpExtensionEquals(extension, ".wasm")) {
        return "application/wasm";
    }
    return "application/octet-stream";
}

}  // namespace ruvia::detail
