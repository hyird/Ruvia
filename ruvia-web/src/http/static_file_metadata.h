#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_hex.h"
#include "ruvia/http/http_response_file.h"
#include "ruvia/web/detail/util/fnv1a.h"

namespace ruvia::detail {

// Above this many entries an index lookup binary-searches instead of scanning.
inline constexpr std::size_t static_root_linear_lookup_limit = 8;

// Looks a key up in an index vector sorted by `projection` (a string_view of
// each entry): a short vector is scanned, a longer one binary-searched.
template <typename entry_type, typename projection_type>
[[nodiscard]] const entry_type* find_static_index_entry(
    const std::pmr::vector<entry_type>& entries, std::string_view key, projection_type projection) noexcept {
    if (entries.size() <= static_root_linear_lookup_limit) {
        for (const auto& entry : entries) {
            if (projection(entry) == key) {
                return &entry;
            }
        }
        return nullptr;
    }

    const auto iter = std::ranges::lower_bound(entries, key, std::ranges::less{}, projection);
    if (iter == entries.end() || projection(*iter) != key) {
        return nullptr;
    }
    return &*iter;
}

// Precompressed sidecars (foo.js.br / .zst / .gz) in selection preference
// order: an Accept-Encoding tie resolves br > zstd > gzip. Indexing,
// precompression and representation selection all read this one table.
struct static_precompression_sidecar final {
    std::string_view suffix_;
    http_content_coding content_coding_;
};

inline constexpr std::array<static_precompression_sidecar, 3> static_precompression_sidecars{{
    {".br", http_content_coding::brotli},
    {".zst", http_content_coding::zstd},
    {".gz", http_content_coding::gzip},
}};

[[nodiscard]] constexpr bool is_precompressed_sidecar_extension(std::string_view extension) noexcept {
    for (const auto& sidecar : static_precompression_sidecars) {
        if (sidecar.suffix_ == extension) {
            return true;
        }
    }
    return false;
}

// Orders two indexed snapshots of one file by (mtime, change token). A sidecar
// older than its identity file is never served: presence alone cannot prove
// its decoded bytes still describe the current resource.
struct static_file_version final {
    std::time_t modified_seconds_;
    std::uint64_t modified_token_;

    [[nodiscard]] constexpr bool at_least_as_new_as(const static_file_version& identity) const noexcept {
        if (modified_seconds_ != identity.modified_seconds_) {
            return modified_seconds_ > identity.modified_seconds_;
        }
        return modified_token_ >= identity.modified_token_;
    }
};

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
    if (static_file_extension_equals(extension, ".apng")) {
        return "image/apng";
    }
    if (static_file_extension_equals(extension, ".avif")) {
        return "image/avif";
    }
    if (static_file_extension_equals(extension, ".webp")) {
        return "image/webp";
    }
    if (static_file_extension_equals(extension, ".bmp")) {
        return "image/bmp";
    }
    if (static_file_extension_equals(extension, ".ico") ||
        static_file_extension_equals(extension, ".cur")) {
        return "image/vnd.microsoft.icon";
    }
    if (static_file_extension_equals(extension, ".woff")) {
        return "font/woff";
    }
    if (static_file_extension_equals(extension, ".woff2")) {
        return "font/woff2";
    }
    if (static_file_extension_equals(extension, ".ttf")) {
        return "font/ttf";
    }
    if (static_file_extension_equals(extension, ".otf")) {
        return "font/otf";
    }
    if (static_file_extension_equals(extension, ".eot")) {
        return "application/vnd.ms-fontobject";
    }
    if (static_file_extension_equals(extension, ".xml") ||
        static_file_extension_equals(extension, ".xsl")) {
        return "text/xml; charset=utf-8";
    }
    if (static_file_extension_equals(extension, ".map")) {
        return "application/json; charset=utf-8";
    }
    if (static_file_extension_equals(extension, ".webmanifest")) {
        return "application/manifest+json; charset=utf-8";
    }
    return "application/octet-stream";
}

[[nodiscard]] inline std::string_view guess_static_file_content_type(
    const std::filesystem::path& path) noexcept {
    return guess_static_file_content_type_from_path_view(
        std::basic_string_view<std::filesystem::path::value_type>(path.native()));
}

// Strong validator for one file snapshot: a quoted 64-bit FNV-1a digest of the
// size, modification token, file identity and (for a stored non-identity
// representation) content coding, so any change to one of them yields a new
// ETag. Hashing keeps the field short and does not disclose the volume and
// file index the identity words carry.
inline constexpr std::size_t static_file_etag_size = 2U + 2U * sizeof(std::uint64_t);

[[nodiscard]] inline std::array<char, static_file_etag_size> format_static_file_snapshot_etag(
    std::uint64_t size, std::uint64_t modified_token, http_response_file_identity identity,
    http_content_coding coding = http_content_coding::identity) noexcept {
    auto hash = fnv1a_accumulate_integer(fnv1a_offset_basis, size);
    hash = fnv1a_accumulate_integer(hash, modified_token);
    for (const auto word : identity.words()) {
        hash = fnv1a_accumulate_integer(hash, word);
    }
    if (coding != http_content_coding::identity) {
        hash = fnv1a_accumulate(hash, http_content_coding_token(coding));
    }
    std::array<unsigned char, sizeof(hash)> digest{};
    for (std::size_t i = 0; i < digest.size(); ++i) {
        digest[i] = static_cast<unsigned char>(hash >> ((digest.size() - 1 - i) * 8U));
    }
    std::array<char, static_file_etag_size> output{};
    output.front() = '"';
    http_encode_lower_hex(output.data() + 1, digest);
    output.back() = '"';
    return output;
}

[[nodiscard]] inline std::pmr::string make_static_file_snapshot_etag(
    std::pmr::memory_resource* resource, std::uint64_t size, std::uint64_t modified_token,
    http_response_file_identity identity, http_content_coding coding = http_content_coding::identity) {
    const auto formatted = format_static_file_snapshot_etag(size, modified_token, identity, coding);
    return std::pmr::string(formatted.data(), formatted.size(), resource);
}

}  // namespace ruvia::detail
