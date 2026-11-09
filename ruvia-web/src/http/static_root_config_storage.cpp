#include "http/static_root_config_storage.h"

#include <algorithm>
#include <iterator>
#include <string_view>

#include "ruvia/http/http_ascii.h"

#include "http/static_file_metadata.h"
namespace ruvia::detail {

namespace {

void lowercase_ascii(std::pmr::string& value) noexcept {
    for (char& character : value) {
        character = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(character)));
    }
}

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

const static_root_mime_type_storage* find_static_mime_type(
    const std::pmr::vector<static_root_mime_type_storage>& mime_types,
    std::string_view extension) noexcept {
    return find_static_index_entry(mime_types, extension,
        [](const static_root_mime_type_storage& mime) noexcept {
            return std::string_view(mime.extension_);
        });
}
}  // namespace

static_root_config_storage::static_root_config_storage(
    const static_root_config_storage& source_value, std::pmr::memory_resource* resource)
    : static_root_config_storage(resource) {
    cache_control_ = source_value.cache_control_;
    index_file_ = source_value.index_file_;
    default_content_type_ = source_value.default_content_type_;
    auto* const stored_resource = cache_control_.get_allocator().resource();
    mime_types_.reserve(source_value.mime_types_.size());
    for (const auto& mime : source_value.mime_types_) {
        auto& stored = mime_types_.emplace_back(stored_resource);
        stored.extension_ = mime.extension_;
        stored.content_type_ = mime.content_type_;
    }
    file_type_kind_ = source_value.file_type_kind_;
    file_type_extensions_.reserve(source_value.file_type_extensions_.size());
    for (const auto& extension : source_value.file_type_extensions_) {
        file_type_extensions_.emplace_back(extension);
    }
    range_requests_ = source_value.range_requests_;
    response_validators_ = source_value.response_validators_;
    dotfiles_ = source_value.dotfiles_;
}

static_root_config_storage store_validated_static_root_config(
    const static_root_options& source_value, std::pmr::memory_resource* resource) {
    static_root_config_storage result(resource);
    result.cache_control_ = source_value.cache_control_.view();
    result.index_file_ = source_value.index_file_.view();
    result.default_content_type_ = source_value.default_content_type_.view();
    auto* const stored_resource = result.cache_control_.get_allocator().resource();
    result.mime_types_.reserve(source_value.mime_types_.size());
    for (const auto& mime : source_value.mime_types_) {
        auto& stored = result.mime_types_.emplace_back(stored_resource);
        const auto ext = mime.extension_.view();
        const bool has_dot = ext.starts_with('.');
        stored.extension_.reserve(ext.size() + (has_dot ? 0U : 1U));
        if (!has_dot) {
            stored.extension_.push_back('.');
        }
        stored.extension_.append(ext);
        lowercase_ascii(stored.extension_);
        stored.content_type_ = mime.content_type_.view();
    }
    std::ranges::sort(result.mime_types_,
        [](const static_root_mime_type_storage& left, const static_root_mime_type_storage& right) {
            return left.extension_ < right.extension_;
        });

    result.file_type_kind_ = source_value.file_types_.kind_;
    if (result.file_type_kind_ == static_file_type_policy::kind::only) {
        result.file_type_extensions_.reserve(source_value.file_types_.extensions_.size());
        for (std::string_view extension : source_value.file_types_.extensions_) {
            if (extension.starts_with('.')) {
                extension.remove_prefix(1);
            }
            auto& stored = result.file_type_extensions_.emplace_back(extension);
            lowercase_ascii(stored);
        }
        std::ranges::sort(result.file_type_extensions_);
        result.file_type_extensions_.erase(std::ranges::unique(result.file_type_extensions_).begin(),
            result.file_type_extensions_.end());
    }
    result.range_requests_ = source_value.range_requests_;
    result.response_validators_ = source_value.response_validators_;
    result.dotfiles_ = source_value.dotfiles_;
    return result;
}

bool is_valid_static_file_extension(std::string_view extension) noexcept {
    if (extension.empty() || (extension.find('/') != std::string_view::npos) ||
        (extension.find('\\') != std::string_view::npos)) {
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
    if (config.file_type_kind_ == static_file_type_policy::kind::all) {
        return true;
    }

    if (extension.empty() || extension == ".") {
        return false;
    }
    const auto value = extension.substr(1);
    if (config.file_type_kind_ == static_file_type_policy::kind::defaults) {
        return std::ranges::binary_search(default_static_file_types, value);
    }
    return std::ranges::binary_search(config.file_type_extensions_, value);
}

std::pmr::string content_type_for(std::string_view extension,
    const static_root_config_storage& config, std::pmr::memory_resource* resource) {
    if (const auto* const mime = find_static_mime_type(config.mime_types_, extension); mime != nullptr) {
        return std::pmr::string(mime->content_type_, resource);
    }

    const auto guessed = guess_static_file_content_type_from_extension(extension);
    if (guessed != std::string_view("application/octet-stream") ||
        config.default_content_type_.empty()) {
        return std::pmr::string(guessed, resource);
    }
    return std::pmr::string(config.default_content_type_, resource);
}

}  // namespace ruvia::detail
