#include "http/static_file_types.h"

#include <algorithm>
#include <iterator>
#include <string_view>

#include "http/static_file_metadata.h"

namespace ruvia {

namespace {

inline constexpr std::size_t static_root_linear_lookup_limit = 8;

inline constexpr std::string_view default_static_file_types[] = {
    "apng",
    "avif",
    "bmp",
    "css",
    "cur",
    "eot",
    "gif",
    "htm",
    "html",
    "ico",
    "jpeg",
    "jpg",
    "js",
    "json",
    "map",
    "mjs",
    "otf",
    "png",
    "svg",
    "ttf",
    "txt",
    "wasm",
    "webmanifest",
    "webp",
    "woff",
    "woff2",
    "xml",
    "xsl",
};

const detail::static_root_mime_type_storage* find_static_mime_type(
    const std::pmr::vector<detail::static_root_mime_type_storage>& mime_types,
    std::string_view extension) noexcept {
    if (mime_types.size() <= static_root_linear_lookup_limit) {
        for (const auto& mime : mime_types) {
            if (mime.extension_ == extension) {
                return &mime;
            }
        }
        return nullptr;
    }

    const auto iter = std::ranges::lower_bound(mime_types, extension, std::ranges::less{},
        [](const detail::static_root_mime_type_storage& mime) noexcept {
            return std::string_view(mime.extension_);
        });
    if (iter == mime_types.end() || std::string_view(iter->extension_) != extension) {
        return nullptr;
    }
    return &*iter;
}

}  // namespace

namespace detail {

bool is_valid_static_file_extension(std::string_view extension) noexcept {
    if (extension.empty() || (extension.find('/') != std::string_view::npos) || (extension.find('\\') != std::string_view::npos)) {
        return false;
    }
    if (extension == "." || extension == "..") {
        return false;
    }
    if (extension.front() == '.') {
        extension.remove_prefix(1);
    }
    return !extension.empty();
}

bool file_type_allowed(std::string_view extension, const static_root_config_storage& config) {
    if (config.file_type_kind_ == static_file_type_policy::kind_type::all) {
        return true;
    }

    if (extension.empty() || extension == ".") {
        return false;
    }
    const auto value = extension.substr(1);
    if (config.file_type_kind_ == static_file_type_policy::kind_type::defaults) {
        return std::ranges::binary_search(default_static_file_types, value);
    }
    return std::ranges::binary_search(config.file_type_extensions_, value);
}

std::pmr::string content_type_for(const std::filesystem::path& path, std::string_view extension,
    const static_root_config_storage& config, std::pmr::memory_resource* resource) {
    if (const auto* const mime = find_static_mime_type(config.mime_types_, extension); mime != nullptr) {
        return std::pmr::string(mime->content_type_, resource);
    }

    const auto guessed = detail::guess_static_file_content_type(path);
    if (guessed != std::string_view("application/octet-stream") ||
        config.default_content_type_.empty()) {
        return std::pmr::string(guessed, resource);
    }
    return std::pmr::string(config.default_content_type_, resource);
}

}  // namespace detail
}  // namespace ruvia
