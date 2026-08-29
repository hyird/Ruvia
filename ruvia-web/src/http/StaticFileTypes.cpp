#include "ruvia/web/detail/http/static/StaticFileTypes.h"

#include <algorithm>
#include <iterator>
#include <string_view>

#include "ruvia/web/detail/http/static/StaticFileMetadata.h"

namespace ruvia {

namespace {

inline constexpr std::size_t kStaticRootLinearLookupLimit = 8;

inline constexpr std::string_view kDefaultStaticFileTypes[] = {
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

const detail::StaticRootMimeTypeStorage* findStaticMimeType(
    const std::pmr::vector<detail::StaticRootMimeTypeStorage>& mimeTypes,
    std::string_view extension) noexcept {
    if (mimeTypes.size() <= kStaticRootLinearLookupLimit) {
        for (const auto& mime : mimeTypes) {
            if (mime.extension == extension) {
                return &mime;
            }
        }
        return nullptr;
    }

    const auto iter = std::ranges::lower_bound(mimeTypes, extension, std::ranges::less{},
        [](const detail::StaticRootMimeTypeStorage& mime) noexcept {
            return std::string_view(mime.extension);
        });
    if (iter == mimeTypes.end() || std::string_view(iter->extension) != extension) {
        return nullptr;
    }
    return &*iter;
}

}  // namespace

namespace detail {

bool isValidStaticFileExtension(std::string_view extension) noexcept {
    if (extension.empty() || extension.find('/') != std::string_view::npos ||
        extension.find('\\') != std::string_view::npos) {
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

bool fileTypeAllowed(std::string_view extension, const StaticRootConfigStorage& config) {
    if (config.fileTypeKind == StaticFileTypePolicy::Kind::kAll) {
        return true;
    }

    if (extension.empty() || extension == ".") {
        return false;
    }
    const auto value = extension.substr(1);
    if (config.fileTypeKind == StaticFileTypePolicy::Kind::kDefaults) {
        return std::ranges::binary_search(kDefaultStaticFileTypes, value);
    }
    return std::ranges::binary_search(config.fileTypeExtensions, value);
}

std::pmr::string contentTypeFor(const std::filesystem::path& path, std::string_view extension,
    const StaticRootConfigStorage& config, std::pmr::memory_resource* resource) {
    if (const auto* const mime = findStaticMimeType(config.mimeTypes, extension); mime != nullptr) {
        return std::pmr::string(mime->contentType, resource);
    }

    const auto guessed = detail::guessStaticFileContentType(path);
    if (guessed != std::string_view("application/octet-stream") ||
        config.defaultContentType.empty()) {
        return std::pmr::string(guessed, resource);
    }
    return std::pmr::string(config.defaultContentType, resource);
}

}  // namespace detail
}  // namespace ruvia
